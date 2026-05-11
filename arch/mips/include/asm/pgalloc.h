/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994 - 2001, 2003 by Ralf Baechle
 * Copyright (C) 1999, 2000, 2001 Silicon Graphics, Inc.
 */
#ifndef _ASM_PGALLOC_H
#define _ASM_PGALLOC_H

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/sched.h>

/*
 * Don't pull in asm-generic/pgalloc.h: pgtable_t is pte_t * on mips
 * (so PGCL can pack many MMUPAGE-sized PTE tables into one PAGE_SIZE
 * chunk), which is incompatible with the generic helpers that return
 * struct page *.  Provide every entry point explicitly here, mirroring
 * sparc64's approach.
 */

#define GFP_PGTABLE_KERNEL	(GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER	(GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

static inline void pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmd,
	pte_t *pte)
{
	set_pmd(pmd, __pmd((unsigned long)pte));
}

/*
 * pgtable_t is pte_t * on mips (see asm/page.h), so pmd_populate is
 * identical to pmd_populate_kernel: install the PTE-table virtual
 * address directly, no struct page round-trip.
 */
static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd,
	pgtable_t pte)
{
	set_pmd(pmd, __pmd((unsigned long)pte));
}

/*
 * Override the asm-generic pmd_pgtable (defined in include/linux/pgtable.h
 * as pmd_page(pmd) — i.e. struct page *) with the pte_t * form, since
 * pgtable_t is pte_t * on mips.  Pre-empt the generic definition.
 */
#undef pmd_pgtable
#define pmd_pgtable(pmd)	((pte_t *)pmd_page_vaddr(pmd))

/*
 * Initialize a new pmd table with invalid pointers.
 */
extern void pmd_init(void *addr);

#ifndef __PAGETABLE_PMD_FOLDED

static inline void pud_populate(struct mm_struct *mm, pud_t *pud, pmd_t *pmd)
{
	set_pud(pud, __pud((unsigned long)pmd));
}
#endif

/*
 * Initialize a new pgd table with invalid pointers.
 */
extern void pgd_init(void *addr);
extern pgd_t *pgd_alloc(struct mm_struct *mm);

static inline void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	pagetable_dtor_free(virt_to_ptdesc(pgd));
}

/* PTE allocator (PGCL sub-page packed) — see arch/mips/mm/pgtable_pte.c */
extern pte_t *pte_alloc_one_kernel(struct mm_struct *mm);
extern pgtable_t pte_alloc_one(struct mm_struct *mm);
extern void pte_free_kernel(struct mm_struct *mm, pte_t *pte);
extern void pte_free(struct mm_struct *mm, pgtable_t pte);

/*
 * The TLB-gather PTE-table free path can't batch sub-table frees through
 * tlb_remove_ptdesc (which would prematurely release the kernel page that
 * still hosts other live sub-tables).  Just call our packed pte_free
 * directly here; the eventual tlb_finish_mmu() flushes the TLB before
 * the freed sub-table can be observed by user mode.
 */
#define __pte_free_tlb(tlb, pte, address)			\
	pte_free((tlb)->mm, (pte))

#ifndef __PAGETABLE_PMD_FOLDED

static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long address)
{
	pmd_t *pmd;
	struct ptdesc *ptdesc;

	ptdesc = pagetable_alloc(GFP_KERNEL_ACCOUNT | __GFP_ZERO,
				 PMD_TABLE_ORDER);
	if (!ptdesc)
		return NULL;

	if (!pagetable_pmd_ctor(mm, ptdesc)) {
		pagetable_free(ptdesc);
		return NULL;
	}

	pmd = ptdesc_address(ptdesc);
	pmd_init(pmd);
	return pmd;
}

static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (MMUPAGE_SIZE - 1));
	pagetable_dtor_free(virt_to_ptdesc(pmd));
}

#define __pmd_free_tlb(tlb, x, addr)	tlb_remove_ptdesc((tlb), virt_to_ptdesc(x))

#endif

#ifndef __PAGETABLE_PUD_FOLDED

static inline pud_t *pud_alloc_one(struct mm_struct *mm, unsigned long address)
{
	pud_t *pud;
	struct ptdesc *ptdesc = pagetable_alloc(GFP_KERNEL | __GFP_ZERO,
						PUD_TABLE_ORDER);

	if (!ptdesc)
		return NULL;
	pagetable_pud_ctor(ptdesc);
	pud = ptdesc_address(ptdesc);

	pud_init(pud);
	return pud;
}

static inline void pud_free(struct mm_struct *mm, pud_t *pud)
{
	BUG_ON((unsigned long)pud & (MMUPAGE_SIZE - 1));
	pagetable_dtor_free(virt_to_ptdesc(pud));
}

static inline void p4d_populate(struct mm_struct *mm, p4d_t *p4d, pud_t *pud)
{
	set_p4d(p4d, __p4d((unsigned long)pud));
}

#define __pud_free_tlb(tlb, x, addr)	tlb_remove_ptdesc((tlb), virt_to_ptdesc(x))

#endif /* __PAGETABLE_PUD_FOLDED */

#endif /* _ASM_PGALLOC_H */
