// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hypervisor call module for userspace
 *
 * Copyright (C) 2021 Digital14 Ltd.
 *
 * Authors:
 * Konsta Karsisto <konsta.karsisto@gmail.com>
 *
 * File: hyp-drv.c
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <asm-generic/bug.h>
#include <asm-generic/ioctls.h>
#include <linux/mm.h>
#include <asm/memory.h>

#include "hvccall-defines.h"
#include "kaddr.h"
#include "hyp-drv.h"

MODULE_DESCRIPTION("Hypervisor call module for userspace");
MODULE_LICENSE("GPL v2");

#define DEVICE_NAME "hyp-drv"
#define ADDR_MASK 0xFFFFFFFFFFFF
#define ROUND_DOWN(N,M) ((N) & ~((M) - 1))
#define KADDR_MASK 0xFFFFFFFFFFUL

#define ats1e1r(va) \
	({ \
		u64 value; \
			__asm__ __volatile__( \
					"at	s1e1r, %[vaddr]\n" \
					"mrs	%[paddr], PAR_EL1\n" \
					: [paddr] "=r"(value) \
					: [vaddr] "r"(va) \
					:); \
		value; \
	})

static int major;
static int dopen;

static u64 kaddr_to_phys(u64 kaddr)
{
	return virt_to_phys((void *)kaddr);

	/* WAS: return ats1e1r(kaddr) & KADDR_MASK; */
}

#define __asmeq(x, y)  ".ifnc " x "," y " ; .err ; .endif\n\t"

static noinline int
call_hyp(u64 function_id, u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4)
{
	register u64 reg0 asm ("x0") = function_id;
	register u64 reg1 asm ("x1") = arg0;
	register u64 reg2 asm ("x2") = arg1;
	register u64 reg3 asm ("x3") = arg2;
	register u64 reg4 asm ("x4") = arg3;
	register u64 reg5 asm ("x5") = arg4;

	__asm__ __volatile__ (
		__asmeq("%0", "x0")
		__asmeq("%1", "x1")
		__asmeq("%2", "x2")
		__asmeq("%3", "x3")
		__asmeq("%4", "x4")
		"hvc	#0\n"
		: "+r"(reg0)
		: "r"(reg1), "r"(reg2), "r"(reg3), "r"(reg4), "r"(reg5)
		: "memory");

	return reg0;
}

static ssize_t
do_host_map(struct hypdrv_mem_region *reg)
{
	u64 section_start;
	u64 section_end;
	u64 size, prot;
	int ret;

	section_start = kaddr_to_phys(reg->start) & ADDR_MASK;
	section_end   = kaddr_to_phys(reg->end) & ADDR_MASK;
	size = ROUND_DOWN(reg->end - reg->start, 0x1000);
	prot = reg->prot;

#ifdef DEBUG
        pr_info("HYPDRV %s: %llx %llx %llx [ %llx %llx %llx ]\n", __func__,
                reg->start, reg->end, prot, section_start, section_end, size);
#endif
	ret = call_hyp(HYP_HOST_MAP_STAGE2, section_start, section_start,
		       size, prot | s2_wb, 0);

	return ret;
}

#define MK_HMR(START, END, PROT)                                \
        (struct hypdrv_mem_region){(u64)START, (u64)END, PROT}

static ssize_t
kernel_lock(void)
{
	struct hypdrv_mem_region reg;
	int err = -ENODEV;

	preempt_disable();
	local_irq_disable();

	/* kernel text section */
	reg = MK_HMR(_text__addr, _etext__addr, HYPDRV_KERNEL_EXEC);
	err = do_host_map(&reg);
	if (err)
		goto out;

	/* rodata */
	reg = MK_HMR(__start_rodata__addr, vdso_start__addr,
		     HYPDRV_PAGE_KERNEL_RO);
	err = do_host_map(&reg);

out:
	local_irq_enable();
	preempt_enable();

#ifdef DEBUG
	pr_info("HYPDRV %s: return %d\n", __func__, err);
#endif

	return err;
}

static int device_open(struct inode *inode, struct file *filp)
{
	if (dopen)
		return -EBUSY;

	dopen = 1;
	return 0;
}

static int device_release(struct inode *inode, struct file *filp)
{
	dopen = 0;

	return 0;
}

static ssize_t
device_read(struct file *filp, char *buffer, size_t length, loff_t *off)
{
	return -ENOTSUPP;
}

static ssize_t
device_write(struct file *filp, const char *buf, size_t len, loff_t *off)
{
	ssize_t res;
	static int locked;

	if (locked)
		return len;

	res = kernel_lock();
	if (res)
		return res;
	locked = 1;

	return len;
}

#ifdef DEBUG
static ssize_t
do_write(struct hypdrv_mem_region *reg)
{
	u64 *section_start;
	u64 *section_end;
	u64 *pos;

	section_start = (u64 *) kaddr_to_phys(reg->start);
	section_end   = (u64 *) kaddr_to_phys(reg->end);

	for (pos = section_start; pos < section_end; pos++)
		*pos = 0xdeadbeef;

	return 0;
}
#endif

static ssize_t
do_read(void __user *argp)
{
	struct log_frag log = { 0 };
	uint64_t res, ret;
	int n;

	res = call_hyp(HYPDRV_READ_LOG, 0, 0, 0, 0, 0);

	n = res & 0xFF;
	if (n == 0 || n > 7)
		return -ENODATA;

	log.frag = res;
	ret = copy_to_user(argp, &log, sizeof(log));

	return ret;
}

int get_region(struct hypdrv_mem_region *reg, void __user *argp)
{
	int ret = 0;

	ret = copy_from_user(reg, argp, sizeof(struct hypdrv_mem_region));

	return ret;
}

static long
device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct hypdrv_mem_region reg;
	void __user *argp = (void __user *) arg;
	int ret = -ENOTSUPP;

	switch (cmd) {
	case HYPDRV_KERNEL_MMAP:
		ret = get_region(&reg, argp);
		if (ret == 0)
			return do_host_map(&reg);
		break;
#ifdef DEBUG
	case HYPDRV_KERNEL_WRITE:
		ret = get_region(&reg, argp);
		if (ret == 0)
			return do_write(&reg);
		break;
#endif
	case HYPDRV_KERNEL_LOCK:
		ret = kernel_lock();
		break;
	case HYPDRV_READ_LOG:
		ret = do_read(argp);
		break;
	case TCGETS:
#ifdef DEBUG
		pr_info("HYPDRV not a TTY\n");
#endif
		ret = -ENOTSUPP;
		break;
	default:
		WARN(1, "HYPDRV unknown ioctl: 0x%x\n", cmd);
	}

	return ret;
}

static const struct file_operations fops = {
	.read = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release,
	.unlocked_ioctl = device_ioctl,
};

int init_module(void)
{
	pr_info("HYPDRV hypervisor driver\n");

	major = register_chrdev(0, DEVICE_NAME, &fops);

	if (major < 0) {
		pr_err("HYPDRV register_chrdev failed with %d\n", major);
		return major;
	}
	pr_info("HYPDRV mknod /dev/%s c %d 0\n", DEVICE_NAME, major);

	return 0;
}

void cleanup_module(void)
{
	if (major > 0)
		unregister_chrdev(major, DEVICE_NAME);
	major = 0;
}
