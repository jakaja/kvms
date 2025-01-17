/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __HYP_API__
#define __HYP_API__
/*
 * Base addressing for data sharing
 */
#define KERNEL_MAP	0xFFFFFFF000000000
#define KERN_VA_MASK	0x0000000FFFFFFFFF
#define CALL_MASK	KERN_VA_MASK
#define KERNEL_BASE	0x4000000000

/*
 * Kernel lock flags
 */
#define HOST_STAGE1_LOCK		0x1
#define HOST_STAGE2_LOCK		0x2
#define HOST_KVM_CALL_LOCK		0x4
#define HOST_PT_LOCK			0x8
#define HOST_KVM_TRAMPOLINE_LOCK	0x10

/*
 * Host protection support
 */
#define HYP_FIRST_HOSTCALL		0x8000
#define HYP_HOST_MAP_STAGE1		0x8000
#define HYP_HOST_MAP_STAGE2		0x8001
#define HYP_HOST_UNMAP_STAGE1		0x8002
#define HYP_HOST_UNMAP_STAGE2		0x8003
#define HYP_HOST_BOOTSTEP		0x8004
#define HYP_HOST_GET_VMID		0x8005
#define HYP_HOST_SET_LOCKFLAGS		0x8006
#define HYP_HOST_PREPARE_STAGE1		0x8007
#define HYP_HOST_PREPARE_STAGE2		0x8008
#define HYP_LAST_HOSTCALL		HYP_HOST_SET_LOCKFLAGS

/*
 * KVM guest support
 */
#define HYP_FIRST_GUESTCALL		0x9000
#define HYP_READ_MDCR_EL2		0x9000
#define HYP_SET_HYP_TXT			0x9001
#define HYP_SET_TPIDR			0x9002
#define HYP_INIT_GUEST			0x9003
#define HYP_FREE_GUEST			0x9004
#define HYP_UPDATE_GUEST_MEMSLOT	0x9005
#define HYP_GUEST_MAP_STAGE2		0x9006
#define HYP_GUEST_UNMAP_STAGE2		0x9007
#define HYP_SET_WORKMEM			0x9008
#define HYP_USER_COPY			0x9009
#define HYP_MKYOUNG			0x900A
#define HYP_SET_GUEST_MEMORY_OPEN	0x900B
#define HYP_SET_GUEST_MEMORY_BLINDED	0x900C
#define HYP_MKOLD			0x900D
#define HYP_ISYOUNG			0x900E
#define HYP_LAST_GUESTCALL		HYP_ISYOUNG

/*
 * Misc
 */
#define HYP_READ_LOG			0xA000

#define STR(x) #x
#define XSTR(s) STR(s)

#ifndef __ASSEMBLY__
extern int __kvms_hvc_cmd(unsigned long cmd, ...);
extern uint64_t __kvms_hvc_get(unsigned long cmd, ...);
#endif // __ASSEMBLY__

#endif // __HYP_API__
