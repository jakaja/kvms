// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <host_platform.h>

#include "hyp_config.h"
#include "armtrans.h"
#include "bits.h"
#include "helpers.h"
#include "spinlock.h"
#include "hvccall.h"
#include "psci.h"
#include "guest.h"
#include "hyplogs.h"
#include "heap.h"
#include "mm.h"
#include "kjump.h"
#include "platform_api.h"

#define CALL_TYPE_UNKNOWN	0
#define CALL_TYPE_HOSTCALL	1
#define CALL_TYPE_GUESTCALL	2
#define CALL_TYPE_KVMCALL	3

typedef int hyp_func_t(void *, ...);
typedef int kvm_func_t(uint64_t, ...);

extern uint64_t __kvm_host_data[PLATFORM_CORE_COUNT];
extern hyp_func_t *__guest_exit;
hyp_func_t *__fpsimd_guest_restore;
extern uint64_t hyp_text_start;
extern uint64_t hyp_text_end;
extern uint64_t core_lock;
uint64_t crash_lock;
uint64_t hostflags;

int set_lockflags(uint64_t flags, uint64_t addr, size_t sz)
{
	if (flags & HOST_STAGE2_LOCK)
		hostflags |= HOST_STAGE2_LOCK;
	if (flags & HOST_STAGE1_LOCK)
		hostflags |= HOST_STAGE1_LOCK;
	if (flags & HOST_KVM_CALL_LOCK)
		hostflags |= HOST_KVM_CALL_LOCK;
	if (flags & HOST_PT_LOCK)
		return lock_host_kernel_area(addr, sz);

	return 0;
}

int is_apicall(uint64_t cn)
{
	if ((cn >= HYP_FIRST_GUESTCALL) &&
	    (cn <= HYP_LAST_GUESTCALL))
		return CALL_TYPE_GUESTCALL;
	if ((cn >= HYP_FIRST_HOSTCALL) &&
	    (cn <= HYP_LAST_HOSTCALL))
		return CALL_TYPE_HOSTCALL;
	return CALL_TYPE_UNKNOWN;
}

int guest_hvccall(register_t cn, register_t a1, register_t a2, register_t a3,
		  register_t a4, register_t a5, register_t a6, register_t a7,
		  register_t a8, register_t a9)
{
	int res = -EINVAL;

	spin_lock(&core_lock);
	switch(cn) {
	case HYP_SET_GUEST_MEMORY_BLINDED:
		res = remove_host_range(a1, a2);
		break;
	case HYP_SET_GUEST_MEMORY_OPEN:
		res = restore_host_range(a1, a2);
		break;
	}
	spin_unlock(&core_lock);

	return res;
}

int hvccall(register_t cn, register_t a1, register_t a2, register_t a3,
	    register_t a4, register_t a5, register_t a6, register_t a7,
	    register_t a8, register_t a9)
{
	kvm_guest_t *guest = NULL;
	int64_t res = -EINVAL;
	bool retried = false;
	hyp_func_t *func;
	uint32_t vmid;
	int ct;

	ct = is_apicall(cn);
	if ((ct == CALL_TYPE_GUESTCALL) && (hostflags & HOST_KVM_CALL_LOCK))
		return -EPERM;

	vmid = get_current_vmid();
	if (vmid != HOST_VMID)
		return guest_hvccall(cn, a1, a2, a3, a4, a5, a6, a7, a8, a9);

	if (ct)
		spin_lock(&core_lock);

	switch (cn) {
	/*
	 * Stage 1 and 2 host side mappings
	 */
	case HYP_HOST_MAP_STAGE1:
		guest = get_guest(HOST_VMID);
		if (!guest) {
			res = -ENOENT;
			break;
		}
		res = mmap_range(guest->s1_pgd, STAGE1, a1, a2, a3, a4,
				 KERNEL_MATTR);
		/*
		 * kern_hyp_va: MSB WATCH
		 *
		LOG("HYP_HOST_MAP_STAGE1: %ld: 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
		     res, a1, a2, a3, a4, a5);
		 */
#ifdef HOSTBLINDING_DEV
		/*
		 * Workaround. Keep mappings of the sections mapped to
		 * el2 intact. Guest appears to map a piece of memory
		 * from a kernel (bss) location mapped by KVM (for still
		 * unknown reason).
		 * We can't make this part of memory unreachable by host.
		 */
		if (add_kvm_hyp_region(a1, a2, a3))
			HYP_ABORT();
#endif // HOSTBLINDING_DEV
		break;
	case HYP_HOST_UNMAP_STAGE1:
		res = unmap_range(NULL, STAGE1, a1, a2);

#ifdef HOSTBLINDING_DEV
		if (remove_kvm_hyp_region(a1))
			ERROR("kvm hyp region not found! %lx\n", a1);
#endif // HOSTBLINDING_DEV
		break;
	/*
	 * HYP_HOST_PREPARE_STAGE2 prepares a range of memory with an existing
	 * stage2 translation table. HYP_HOST_PREPARE_STAGE2 does not change
	 * the memory attributes as normal stage2 mapping operation may do, but
	 * instead it only tears the possible contiguous areas that interleave
	 * the range to be prepared. If the prepared area boundaries interleave
	 * with existing block mappings the block will be split to align with
	 * the mapped area.
	 *
	 * If you don't see the use for the API, don't use it. The primary use
	 * is to avoid issues with a centralized TCU during the system runtime
	 * when the mappings change.
	 *
	 * HYP_HOST_PREPARE_STAGE2 can be called with similar parameters as
	 * HYP_HOST_MAP_STAGE2.
	 */
	case HYP_HOST_PREPARE_STAGE2:
		guest = get_guest(HOST_VMID);
		if (!guest) {
			res = -ENOENT;
			break;
		}
		res = mmap_range(guest->s2_pgd, STAGE2, a1, a2, a3, a4,
				 KEEP_MATTR);
		break;
	case HYP_HOST_MAP_STAGE2:
		guest = get_guest(HOST_VMID);
		if (!guest) {
			res = -ENOENT;
			break;
		}
		res = mmap_range(guest->s2_pgd, STAGE2, a1, a2, a3, a4,
				 KERNEL_MATTR);
		break;
	case HYP_HOST_BOOTSTEP:
	/*	res = hyp_bootstep(a1, a2, a3, a4, a5, a6);*/
		res = 0;
		break;
	case HYP_HOST_GET_VMID:
		res = platform_get_next_vmid(a2);
		guest_set_vmid((void *)a1, res);
		break;
	case HYP_HOST_SET_LOCKFLAGS:
		res = set_lockflags(a1, a2, a3);
		break;
	/*
	 * Control functions
	 */
	case HYP_READ_MDCR_EL2:
		res = read_reg(MDCR_EL2);
		break;
	case HYP_SET_HYP_TXT:
		hyp_text_start = (uint64_t)kern_hyp_va((void *)a1);
		hyp_text_end = (uint64_t)kern_hyp_va((void *)a2);
		__guest_exit = (hyp_func_t *)(a3 & CALL_MASK);
		__fpsimd_guest_restore = (hyp_func_t *)(a4 & CALL_MASK);

		if (hyp_text_end <= hyp_text_start)
			HYP_ABORT();
		if (!__guest_exit)
			HYP_ABORT();

		LOG("hyp text is at 0x%lx - 0x%lx\n", hyp_text_start,
						      hyp_text_end);
		LOG("guest exit is at offset 0x%lx\n", (uint64_t)__guest_exit);
		LOG("simd_guest_restore is at offset 0x%lx\n",
			(uint64_t)__fpsimd_guest_restore);

		res = 0;
		break;
	case HYP_SET_WORKMEM:
		res = set_heap(kern_hyp_va((void *)a1), (size_t)a2);
		break;
	case HYP_SET_TPIDR:
		if ((a2 < 0) || (a2 >= PLATFORM_CORE_COUNT)) {
			ERROR("invalid cpu id %lu\n", a2);
			break;
		}
		__kvm_host_data[a2] = (uint64_t)a3;
		write_reg(TPIDR_EL2, a1);
		res = 0;
		break;
	/*
	 * Guest functions
	 *	- s2 map to establish the machine model
	 *	- unmap, called by linux mm to reclaim pages
	 *	- init, free guest
	 */
	case HYP_GUEST_MAP_STAGE2:
		guest = get_guest(a1);
		if (!guest) {
			res = -ENOENT;
			break;
		}
		res = guest_map_range(guest, a2, a3, a4, a5);
		break;
	case HYP_GUEST_UNMAP_STAGE2:
		guest = get_guest(a1);
		if (!guest) {
			res = -ENOENT;
			break;
		}
		res = guest_unmap_range(guest, a2, a3, a4);
		break;
	case HYP_MKYOUNG:
	case HYP_MKOLD:
	case HYP_ISYOUNG:
		res = guest_stage2_access_flag(cn, a1, a2, a3);
		break;
	case HYP_INIT_GUEST:
		res = init_guest((void *)a1);
		break;
	case HYP_FREE_GUEST:
		res = free_guest((void *)a1);
		break;
	case HYP_UPDATE_GUEST_MEMSLOT:
		res = update_memslot((void *)a1, (kvm_memslot *)a2,
				     (kvm_userspace_memory_region *)a3);
		break;
	case HYP_USER_COPY:
		res = guest_user_copy(a6, a1, a2);
		break;
	/*
	 * Unlocked misc calls
	 */
	case HYP_READ_LOG:
		res = read_log();
		break;
	/*
	 * KVM callbacks
	 */
	default:
		cn = (uint64_t)kern_hyp_va((void *)cn);
do_retry:
		if (is_jump_valid(cn)) {
			func = (hyp_func_t *)cn;
			res = func((void *)a1, a2, a3, a4, a5, a6, a7, a8, a9);
		} else {
			if ((cn >= hyp_text_start) && (cn < hyp_text_end) &&
			   !(hostflags & HOST_KVM_TRAMPOLINE_LOCK) &&
			   !retried) {
				res = add_jump(cn);
				if (!res) {
					retried = true;
					goto do_retry;
				}
			}
			res = -EPERM;
		}
		break;
	}
	if (ct)
		spin_unlock(&core_lock);

	return res;
}

void print_abort(void)
{
	kvm_guest_t *host = NULL;
	uint64_t ipa, pa, far;
	uint64_t ttbr1_el1;

	ttbr1_el1 = (read_reg(TTBR1_EL1) & TTBR_BADDR_MASK);
	host = get_guest(HOST_VMID);
	far = read_reg(FAR_EL2);

	ERROR("VTTBR_EL2 (0x%012x) ESR_EL2 (0x%012lx) FAR_EL2 (0x%012lx)\n",
	      read_reg(VTTBR_EL2), read_reg(ESR_EL2), read_reg(FAR_EL2));
	ERROR("HPFAR_EL2 (0x%012lx)\n", read_reg(HPFAR_EL2));

	ERROR("Host s2 table (0x%012lx)\n", host->s2_pgd);
	/* Walk IPA from host s1 table */
	if (ttbr1_el1 != 0) {
		ipa = pt_walk((struct ptable *)ttbr1_el1,
			       far, NULL, TABLE_LEVELS);
		/* Walk PA from host s2 table */
		pa = pt_walk((struct ptable *)host->s2_pgd,
			      ipa, NULL, TABLE_LEVELS);

		ERROR("FAR: (0x%012lx) IPA: (0x%012lx) PA: (0x%012lx)\n",
		      far, ipa, pa);
	}
}

NORETURN
void hyp_abort(const char *func, const char *file, int line)
{
	ERROR("Aborted: %s:%lu func %s\n", file, line, func);

#ifdef CRASHDUMP
	print_tables(get_current_vmid());
#endif
	while (1)
		wfi();
}

NORETURN
void dump_state(uint64_t level, void *sp)
{
	register uint64_t faddr;
	register uint64_t stage2;
	uint64_t *__frame = (uint64_t *)sp;

	/* Try to make sure the dump stays readable */
	spin_lock(&crash_lock);

	faddr = read_reg(ELR_EL2);
	switch (level) {
	case 1:
		ERROR("Unhandled exception in EL1 at 0x%012lx\n", faddr);

		stage2 = read_reg(VTTBR_EL2) & 0xFFFFFFFFFFFEUL;
		ERROR("Mapping %012lx -> %012lx\n", faddr,
		      pt_walk((struct ptable *)stage2, faddr, NULL, TABLE_LEVELS));
		break;
	case 2:
		ERROR("Unhandled exception in EL2 at 0x%012lx\n", faddr);
		break;
	case 3:
		ERROR("Unhandled SMC trap at 0x%012lx\n", faddr);
		break;
	default:
		ERROR("Unhandled exception\n");
		break;
	}
	ERROR("VTTBR_EL2 (0x%012x) ESR_EL2 (0x%012lx) FAR_EL2 (0x%012lx)\n",
	      read_reg(VTTBR_EL2), read_reg(ESR_EL2), read_reg(FAR_EL2));
	ERROR("HPFAR_EL2 (0x%012lx)\n", read_reg(HPFAR_EL2));

	ERROR("x00(0x%012lx):x01(0x%012lx):x02(0x%012lx):x03(0x%012lx)\n",
		__frame[0], __frame[1], __frame[2], __frame[3]);
	ERROR("x04(0x%012lx):x05(0x%012lx):x06(0x%012lx):x07(0x%012lx)\n",
		__frame[4], __frame[5], __frame[6], __frame[7]);
	ERROR("x08(0x%012lx):x09(0x%012lx):x10(0x%012lx):x11(0x%012lx)\n",
		__frame[8], __frame[9], __frame[10], __frame[11]);
	ERROR("x12(0x%012lx):x13(0x%012lx):x14(0x%012lx):x15(0x%012lx)\n",
		__frame[12], __frame[13], __frame[14], __frame[15]);
	ERROR("x16(0x%012lx):x17(0x%012lx):x18(0x%012lx):x19(0x%012lx)\n",
		__frame[16], __frame[17], __frame[18], __frame[19]);
	ERROR("x20(0x%012lx):x21(0x%012lx):x22(0x%012lx):x23(0x%012lx)\n",
		__frame[20], __frame[21], __frame[22], __frame[23]);
	ERROR("x24(0x%012lx):x25(0x%012lx):x26(0x%012lx):x27(0x%012lx)\n",
		__frame[24], __frame[25], __frame[26], __frame[27]);
	ERROR("x28(0x%012lx):x29(0x%012lx):x30(0x%012lx)\n",
		__frame[28], __frame[29], __frame[30]);

#ifdef CRASHDUMP
	print_tables(get_current_vmid());
#endif
	spin_unlock(&crash_lock);
	while (1)
		wfi()
}
