/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_PAGE_H
#define __VDSO_PAGE_H

#include <uapi/linux/const.h>

/*
 * MMUPAGE_SHIFT is the hardware MMU page shift (from arch Kconfig).
 * PAGE_MMUSHIFT is the extra shift for page clustering (Hugh Dickins, 2001).
 * PAGE_SHIFT = MMUPAGE_SHIFT + PAGE_MMUSHIFT.
 *
 * When PAGE_MMUSHIFT == 0 (default), PAGE_SIZE == MMUPAGE_SIZE and the
 * kernel behaves identically to a stock configuration.
 *
 * When PAGE_MMUSHIFT > 0, the kernel allocates memory in PAGE_SIZE
 * chunks but maps individual MMUPAGE_SIZE PTEs to userspace, preserving
 * the standard TLB/MMU base page ABI.
 */
#define MMUPAGE_SHIFT	CONFIG_PAGE_SHIFT
#define PAGE_MMUSHIFT	CONFIG_PAGE_MMUSHIFT
#define PAGE_MMUCOUNT	(_AC(1,UL) << PAGE_MMUSHIFT)

#define MMUPAGE_SIZE	(_AC(1,UL) << MMUPAGE_SHIFT)

#if !defined(CONFIG_64BIT)
#define MMUPAGE_MASK	(~((1 << MMUPAGE_SHIFT) - 1))
#else
#define MMUPAGE_MASK	(~(MMUPAGE_SIZE - 1))
#endif

#define PAGE_SHIFT	(MMUPAGE_SHIFT + PAGE_MMUSHIFT)

#define PAGE_SIZE	(_AC(1,UL) << PAGE_SHIFT)

#if !defined(CONFIG_64BIT)
/*
 * Applies only to 32-bit architectures.
 *
 * Subtle: (1 << PAGE_SHIFT) is an int, not an unsigned long.
 * So if we assign PAGE_MASK to a larger type it gets extended the
 * way we want (i.e. with 1s in the high bits) while masking a
 * 64-bit value such as phys_addr_t.
 */
#define PAGE_MASK	(~((1 << PAGE_SHIFT) - 1))
#else
#define PAGE_MASK	(~(PAGE_SIZE - 1))
#endif

#endif	/* __VDSO_PAGE_H */
