#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "armtrans.h"
#include "helpers.h"
#include "host_platform.h"
#include "hyplogs.h"
#include "bits.h"
#include "guest.h"
#include "hvccall.h"

#define PHYS_OFFSET 0x40000000UL
#define VIRT_UART 0x09000000UL

#define UART01x_FR_BUSY 0x008
#define UART01x_FR 0x18 /* Flag register (Read only). */
#define UART01x_DR 0x00 /* Data read or written from the interface. */
#define UART01x_RSR 0x04 /* Receive status register (Read). */

uint8_t __stack[STACK_SIZE * PLATFORM_CORE_COUNT] ALIGN(PAGE_SIZE) DATA;

int _IO_putc (int c, FILE *fp);

static bool init_ready;

typedef struct {
	uint64_t addr;
	uint64_t len;
} memmap;

static const memmap base_memmap[] = {
	{ 0, 0x08000000UL },
	{ 0x08000000UL, 0x00010000UL },
	{ 0x08010000UL, 0x00010000UL },
	{ 0x08020000UL, 0x00001000UL },
	{ 0x08030000UL, 0x00010000UL },
	{ 0x08040000UL, 0x00010000UL },
	{ 0x08080000UL, 0x00020000UL },
	{ 0x080A0000UL, 0x00F60000UL },
	{ VIRT_UART, 0x00001000UL },
	{ 0x09010000UL, 0x00001000UL },
	{ 0x09020000UL, 0x00001000UL },
	{ 0x09030000UL, 0x00001000UL },
	{ 0x09040000UL, 0x00001000UL },
	{ 0x09050000UL, 0x00020000UL },
	{ 0x09070000UL, 0x00001000UL },
	{ 0x09080000UL, 0x00001000UL },
	{ 0x0a000000UL, 0x00004000UL },
	{ 0x0c000000UL, 0x02000000UL },
	{ 0x0e000000UL, 0x01000000UL },
	{ 0x10000000UL, 0x2eff0000UL },
	{ 0x3eff0000UL, 0x00010000UL },
	{ 0x3f000000UL, 0x01000000UL },
	{ 0, 0 },
};

int machine_virt(kvm_guest_t *host)
{
	int stage = STAGE1, res = 0, i;
	uint64_t perms, type;
	struct ptable *pgd;

nextmap:
	i = 0;
	while (base_memmap[i].len) {
		if (stage == STAGE2) {
			perms = PAGE_HYP_RW;
			type = S2_DEV_NGNRE;
			pgd = host->s2_pgd;
		} else {
			perms = PAGE_KERNEL_RW;
			type = DEVICE_MEMORY;
			pgd = host->s1_pgd;
		}

		res = mmap_range(pgd, stage, base_memmap[i].addr,
				 base_memmap[i].addr, base_memmap[i].len,
				 perms, type);
		if (res)
			goto error;
		i++;
	}
	if (stage == STAGE1) {
		stage = STAGE2;
		goto nextmap;
	}
	perms = PAGE_KERNEL_RWX;
	res = mmap_range(host->s1_pgd, STAGE1, PHYS_OFFSET, PHYS_OFFSET,
			 SZ_1G * 4, perms, NORMAL_MEMORY);
	if (res)
		goto error;

	perms = PAGE_HYP_RWX;
	res = mmap_range(host->s2_pgd, STAGE2, PHYS_OFFSET, PHYS_OFFSET,
			 SZ_1G * 3, perms, S2_NORMAL_MEMORY);
	if (res)
		goto error;

	/* Virt is a debug target, dump. */
	print_mappings(HOST_VMID, STAGE1, 0, SZ_1G * 5);
	print_mappings(HOST_VMID, STAGE2, 0, SZ_1G * 5);

error:
	LOG("virt initialization return: %x\n\n", res);
	return res;
}

#ifdef DEBUG
int _IO_putc(int c, struct _IO_FILE *__fp)
{
	uint8_t *uart = (uint8_t *)VIRT_UART;

	*(uart + UART01x_DR) = c;
	while (*(uart + UART01x_FR) & UART01x_FR_BUSY) {
	}

	return 0;
}
#endif

int console_putc(unsigned char c)
{
	return _IO_putc((int)c, NULL);
}

int machine_init(kvm_guest_t *host)
{
	int res;

	init_ready = false;
	res = machine_virt(host);
	init_ready = true;

	return res;
}

bool machine_init_ready(void)
{
	return init_ready;
}

int platform_init_host_pgd(kvm_guest_t *host)
{

	if (!host)
		return -EINVAL;

	host->s1_pgd = alloc_table(HOST_VMID);
	host->s2_pgd = alloc_table(HOST_VMID);

	if (!host->s1_pgd || !host->s2_pgd)
		return -ENOMEM;

	return 0;
}

void platform_early_setup(void)
{
	uint64_t hcr_el2, cnthctl_el2;

	/* 64 bit only, Trap SMCs */
	hcr_el2 = 0;
	bit_set(hcr_el2, HCR_RW_BIT);
	bit_set(hcr_el2, HCR_VM_BIT);
	bit_set(hcr_el2, HCR_NV2_BIT);
	// bit_set(hcr_el2, hcr_tsc_bit);
	write_reg(HCR_EL2, hcr_el2);

	/* EL1 timer access */
	cnthctl_el2 = 0;
	bit_set(cnthctl_el2, CNTHCTL_EL1PCTEN_BIT);
	bit_set(cnthctl_el2, CNTHCTL_EL1PCEN_BIT);
	bit_set(cnthctl_el2, CNTHCTL_ENVTEN_BIT);
	write_reg(CNTHCTL_EL2, cnthctl_el2);
	write_reg(CNTVOFF_EL2, 0);

	/* Processor id */
	write_reg(VPIDR_EL2, read_reg(MIDR_EL1));

	/* Use linux mair */
	write_reg(MAIR_EL2, PLATFORM_MAIR_EL2);

	isb();
}

void platform_mmu_prepare(void)
{
	kvm_guest_t *host;

	if (PLATFORM_VTCR_EL2 != 0)
		write_reg(VTCR_EL2, PLATFORM_VTCR_EL2);

	if (PLATFORM_TCR_EL2 != 0)
		write_reg(TCR_EL2, PLATFORM_TCR_EL2);

	host = get_guest(HOST_VMID);
	if (!host)
		HYP_ABORT();

	write_reg(TTBR0_EL2, (uint64_t)host->s1_pgd);
	write_reg(VTTBR_EL2, (uint64_t)host->s2_pgd);
	set_current_vmid(HOST_VMID);
	host->table_levels = TABLE_LEVELS;

	dsb();
	isb();
}

uint32_t platform_get_next_vmid(uint32_t next_vmid)
{
	int i;
	kvm_guest_t *guest;

	if (next_vmid < GUEST_VMID_START)
		next_vmid = GUEST_VMID_START;

	for (i = next_vmid; i < PRODUCT_VMID_MAX; i++) {
		guest = get_guest(i);
		if (!guest) {
			next_vmid = i;
			break;
		}
	}
	return next_vmid;
}

void platform_console_init(void)
{
	/* placeholder */
}

uint8_t *platfrom_get_stack_ptr(uint64_t init_index)
{
	return &__stack[(STACK_SIZE * init_index) + STACK_SIZE];
}
