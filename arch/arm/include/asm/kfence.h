/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_ARM_KFENCE_H
#define __ASM_ARM_KFENCE_H

#include <linux/kfence.h>

#include <asm/pgalloc.h>
#include <asm/set_memory.h>

static inline int split_pmd_page(pmd_t *pmd, unsigned long addr)
{
	int i;
	unsigned long pfn = PFN_DOWN(__pa(addr));
	pte_t *pte = pte_alloc_one_kernel(&init_mm);

	if (!pte)
		return -ENOMEM;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		phys_addr_t phys = ((phys_addr_t)pfn << PAGE_SHIFT) +
				   (unsigned long)i * MMUPAGE_SIZE;
		set_pte_ext(pte + i, __pte(phys | pgprot_val(PAGE_KERNEL)), 0);
	}
	pmd_populate_kernel(&init_mm, pmd, pte);

	flush_tlb_kernel_range(addr, addr + PMD_SIZE);
	return 0;
}

static inline bool arch_kfence_init_pool(void)
{
	unsigned long addr;
	pmd_t *pmd;

	for (addr = (unsigned long)__kfence_pool; is_kfence_address((void *)addr);
	     addr += MMUPAGE_SIZE) {
		pmd = pmd_off_k(addr);

		if (pmd_leaf(*pmd)) {
			if (split_pmd_page(pmd, addr & PMD_MASK))
				return false;
		}
	}

	return true;
}

static inline bool kfence_protect_page(unsigned long addr, bool protect)
{
	set_memory_valid(addr, 1, !protect);

	return true;
}

#endif /* __ASM_ARM_KFENCE_H */
