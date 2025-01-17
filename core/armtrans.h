/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARMTRANS_H__
#define __ARMTRANS_H__

#include <stdint.h>

#include "include/generated/uapi/linux/version.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
#define TABLE_LEVELS    4
#else
#define TABLE_LEVELS    3
#endif

/*
 * Stage-1 S2AP
 *    EL1        EL0
 * 00 Read/write None
 * 01 Read/write Read/write
 * 10 Read-only  None
 * 11 Read-only  Read-only
 *
 * Stage-2 S2AP
 * 00 None
 * 01 Read-only
 * 10 Write-only
 * 11 Read/write
 */

#define PROT_MASK_STAGE1	0x600000000003E0
#define PROT_MASK_STAGE2	0x6A0000000003C0
#define TYPE_MASK_STAGE1	0x1C
#define TYPE_MASK_STAGE2	0x3C
#define TYPE_SHIFT		2
#define VADDR_MASK		0xFFFFFFFFFFFFUL
#define PAGE_SHARED		0x40000000000040
#define PAGE_SHARED_EXEC	0x00000000000040


/*
 * FIXME: we need more human readable permission bits.
 */

/* Stage 1 */
#define PAGE_KERNEL_RW		0x40000000000000
#define PAGE_KERNEL_RWX		0x00000000000000
#define PAGE_KERNEL_RO		0x40000000000080
#define PAGE_KERNEL_EXEC	0x00000000000080

/* Stage 2 */
#define S2AP_SHIFT		6
#define S2AP_NONE		(0x0UL << S2AP_SHIFT)
#define S2AP_READ		(0x1UL << S2AP_SHIFT)
#define S2AP_WRITE		(0x2UL << S2AP_SHIFT)
#define S2AP_RW			(0x3UL << S2AP_SHIFT)
#define S2AP_MASK		(0x3UL << S2AP_SHIFT)

#define S2_EXEC_SHIFT		53
#define S2_EXEC_EL1EL0		(0x0UL << S2_EXEC_SHIFT)
#define S2_EXEC_EL0		(0x1UL << S2_EXEC_SHIFT)
#define S2_EXEC_NONE		(0x2UL << S2_EXEC_SHIFT)
#define S2_EXEC_EL1		(0x3UL << S2_EXEC_SHIFT)
#define S2_EXEC_MASK		(0x3UL << S2_EXEC_SHIFT)

#define PAGE_HYP_RW		0x400000000000c0
#define PAGE_HYP_RWX		0x000000000000c0
#define PAGE_HYP_RO		0x40000000000040
#define PAGE_HYP_EXEC		0x00000000000040
#define PAGE_HYP_DEVICE		0x400000000000c0

#define STAGE1 0
#define STAGE2 1

/*
 * Stage 1 MAIR_EL2 slot. Standard linux allocation on
 * virt, platform specific otherwise.
 */
#define DEVICE_STRONGORDER	(PLAT_DEVICE_STRONGORDER << TYPE_SHIFT)
#define DEVICE_ORDER		(PLAT_DEVICE_ORDER << TYPE_SHIFT)
#define DEVICE_GRE		(PLAT_DEVICE_GRE << TYPE_SHIFT)
#define NORMAL_NOCACHE		(PLAT_NORMAL_NOCACHE << TYPE_SHIFT)
#define NORMAL_WBACK_P		(PLAT_NORMAL_WBACK_P << TYPE_SHIFT)
#define NORMAL_WT_P		(PLAT_NORMAL_WT_P << TYPE_SHIFT)
#define NORMAL_MEMORY NORMAL_WBACK_P
#define DEVICE_MEMORY DEVICE_ORDER
#define INVALID_MEMORY		1 << 6
#define KERNEL_MATTR		1 << 7
#define KEEP_MATTR			1 << 8

/* Shareability SH [9:8], Stage 1 and 2 */
#define SH_SHIFT		0x8
#define SH_NO			0x0
#define SH_OUT			0x2
#define SH_INN			0x3

/* Stage 2 MemAttr[3:2] */
#define S2_DEVICE		(0x0 << TYPE_SHIFT)
#define S2_ONONE		(0x4 << TYPE_SHIFT)
#define S2_OWT			(0x8 << TYPE_SHIFT)
#define S2_OWB			(0xC << TYPE_SHIFT)
/* Stage 2 MemAttr[1:0] Meaning when MemAttr[3:2] == 0b00 */
#define NGNRNE		(0x0 << TYPE_SHIFT)
#define NGNRE		(0x1 << TYPE_SHIFT)
#define NGRE		(0x2 << TYPE_SHIFT)
#define GRE		(0x3 << TYPE_SHIFT)
/* Stage 2 MemAttr[1:0] Meaning when MemAttr[3:2] != 0b00 */
/* Inner Non-cacheable */
#define S2_INONE		(0x1 << TYPE_SHIFT)
/* Inner Write-Through Cacheable */
#define S2_IWT			(0x2 << TYPE_SHIFT)
/* Inner Write-Back Cacheable */
#define S2_IWB			(0x3 << TYPE_SHIFT)

/* Stage 2 normal memory attributes */
#define S2_NORMAL_MEMORY	(S2_OWB | S2_IWB)

/* Stage 2 device memory attributes */
#define S2_DEV_NGNRNE		(S2_DEVICE | NGNRNE)
#define S2_DEV_NGNRE		(S2_DEVICE | NGNRE)
#define S2_DEV_NGRE		(S2_DEVICE | NGRE)
#define S2_DEV_GRE		(S2_DEVICE | GRE)

#define TTBR_BADDR_MASK	0x0000FFFFFFFFFFFEUL
#define MAX_CONTIGUOUS	128

void tdinfo_init(void);

/**
 * Reset current host page tables.
 */
void table_init(void);

/**
 * Enable mmu for the current host VM. machine_init() must have been
 * invoked first to populate the host page tables.
 */
void enable_mmu(void);

/**
 * Allocate a page table structure
 *
 * @param vmid to allocate for
 * @return page table pointer or NULL if out of memory
 */
struct ptable *alloc_table(uint32_t vmid);

/**
 * Free a page table structure
 *
 * @param page table pointer to free
 * @return zero on success or negative error code on failure
 */
int free_table(struct ptable *table);

/**
 * Free all given guest page tables
 *
 * @param vmid vmid to clear
 * @return zero on success or negative error code on failure
 */
int free_guest_tables(uint64_t vmid);

/**
 * Generic page table walk function. Resolve a physical address or IPA
 * of a given virtual address for given PGD.
 *
 * @param vaddr virtual address to resolve
 * @param ptep uint64_t pointer or NULL
 * @param levels depth of the page table walk
 * @return physical address and page permissions in **ptep, ~0UL on error
 */
uint64_t pt_walk(struct ptable *table, uint64_t vaddr, uint64_t **ptep,
		 uint64_t levels);


/**
 * Prevents the calling kernel from ever changing its internal memory
 * area EL1 mappings.
 *
 * @param vaddr virtual address base
 * @param size size of the range
 * @return zero on success or negative error code on failure
 */
int lock_host_kernel_area(uint64_t addr, size_t size);

/**
 * Print memory mappings for given guest to console/log
 *
 * @param vmid vmid of the guest to dump
 * @param stage STAGE1 or STAGE2 of the address translation
 * @param vaddr start virtual address
 * @param sz length of the dump
 * @return void
 */
void print_mappings(uint32_t vmid, uint64_t stage, uint64_t vaddr, size_t sz);

/**
 * Print page tables for given vmid to console/log
 *
 * @param vmid, vmid to dump
 * @return void
 */
void print_tables(uint64_t vmid);

/**
 * Generic stage-1 and -2 map function
 *
 * @param pgd PGD pointer to map into. NULL is interpreted to mean host.
 * @param stage STAGE1 or STAGE2
 * @param vaddr virtual address to map
 * @param paddr physical address to map to
 * @param length page aligned length of the mapping
 * @param prot memory protection attributes (see above)
 * @param type type of the memory. In case of KERNEL_MATTR
 *             the type is embedded in prot parameter.
 * @return zero or success or negative error code on failure
 */
int mmap_range(struct ptable *pgd, uint64_t stage, uint64_t vaddr,
	       uint64_t paddr, size_t length, uint64_t prot, uint64_t type);

/**
 * Generic stage-1 and -2 unmap function
 *
 * @param pgd PGD pointer to unmap from. NULL is interpreted to mean host.
 * @param stage STAGE1 or STAGE2
 * @param vaddr virtual address to unmap
 * @param length page aligned length to unmap
 * @return zero or success or negative error code on failure
 */
int unmap_range(struct ptable *pgd, uint64_t stage, uint64_t vaddr,
		size_t length);

#endif // __ARMTRANS_H__
