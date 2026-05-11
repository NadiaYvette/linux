// SPDX-License-Identifier: GPL-2.0
/*
 * PGCL sub-page packing of user/kernel PTE tables.
 *
 * Under PGCL the kernel allocator hands out PAGE_SIZE chunks while a
 * hardware PTE table is only MMUPAGE_SIZE.  At PAGE_MMUSHIFT=6 a
 * naive pte_alloc_one() wastes 252KB of every 256KB allocation, so a
 * fork-heavy workload trivially eats hundreds of megabytes of pgtables
 * (the OOM dump on mips64 malta -m2G shows pagetables:739MB, ~35% of
 * total RAM).
 *
 * This file packs PAGE_MMUCOUNT (1<<PAGE_MMUSHIFT) MMUPAGE-sized PTE
 * sub-tables into each ptdesc, mirroring m68k's get_pointer_table /
 * free_pointer_table pattern.  All sub-tables in one ptdesc share the
 * single ptl_lock allocated by pagetable_pte_ctor() — a controlled loss
 * of locking granularity in exchange for a PAGE_MMUCOUNT-x reduction in
 * pgtable footprint.
 *
 * pgtable_t is pte_t * on mips (see asm/page.h), so a sub-table
 * pte_t * is a first-class pgtable_t — pmd_populate() installs it
 * directly without any struct page round-trip.
 *
 * NR_PAGETABLE accounting (set by pagetable_pte_ctor / cleared by
 * pagetable_dtor_free) is per-ptdesc, so it correctly reflects the
 * underlying kernel-page footprint, not sub-table count.
 */

#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/gfp.h>
#include <linux/bug.h>
#include <linux/bitops.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

#define PTE_TABLE_SIZE	MMUPAGE_SIZE
/*
 * Packing is disabled (PTE_PER_PAGE=1) until the reuse-path bug that
 * silently hangs init under PAGE_MMUSHIFT=6 is diagnosed.
 *
 * Phase 2 TODO -- what is known so far:
 *   - Typedef change (pgtable_t = pte_t *) is sound: with PTE_PER_PAGE
 *     set to PAGE_MMUCOUNT but the free-list reuse path skipped (every
 *     allocation takes a fresh kernel page, returns slot 0), boot
 *     completes and LTP runs (51p/20f/30s on PAGE_MMUSHIFT=6 malta -m2G).
 *   - As soon as ANY sub-table at offset != 0 is returned (verified
 *     with PTE_PER_PAGE=2: only slot 1 reusable per page), init
 *     silently hangs at "/bin/busybox started with executable stack".
 *     No oops, no panic, no further kernel output.  Removed the
 *     mid-gather tlb_flush_mmu_tlbonly from __pte_free_tlb -- no help.
 *   - pte_lockptr / ptep_lockptr correctly resolve sub-tables to the
 *     shared ptdesc via virt_to_ptdesc -- ruled out as the cause.
 *   - pmd_pfn would mis-compute on regular (non-huge) PMDs but isn't
 *     called on regular PMDs in any hot path -- ruled out.
 *
 * Suspects to test next session:
 *   1. dcache aliasing when sub-tables are accessed via xkphys but
 *      were written via the original kernel-page virtual mapping (mips
 *      sets shm_align_mask precisely because of this).  Try a flush_
 *      cache_range across the sub-table address before returning it.
 *   2. Per-sub-table side-allocated ptl_locks instead of one shared
 *      ptl per kernel page.  Some generic mm path may assume the
 *      ptl-vs-table relationship is 1:1.
 *   3. set_ptes / pte_clear-batched paths assuming PTE table starts at
 *      page boundary -- audit arch/mips/mm/pgtable*.c.
 */
#define PTE_PER_PAGE	1
#define PTE_MARK_FREE	(PTE_PER_PAGE == BITS_PER_LONG ? ~0UL :		\
			 (1UL << PTE_PER_PAGE) - 1UL)

/*
 * Free list of ptdescs that still have at least one free sub-table.
 * pt_index holds a bitmap: bit N set ⇒ sub-table N (at offset
 * N*MMUPAGE_SIZE within the ptdesc's kernel page) is free.
 *
 * Separate lists for user and kernel ptes: kernel ptes never need a
 * ptlock and live in init_mm so we want to keep their accounting and
 * their lifetimes apart from user ptes.
 */
static DEFINE_SPINLOCK(pte_pack_lock);
static LIST_HEAD(pte_user_free_list);
static LIST_HEAD(pte_kernel_free_list);

static inline int ptdesc_sub_index(unsigned long pte_addr)
{
	return (pte_addr & ~PAGE_MASK) >> MMUPAGE_SHIFT;
}

static pte_t *pte_alloc_packed(struct mm_struct *mm,
			       struct list_head *free_list,
			       gfp_t gfp,
			       bool kernel)
{
	struct ptdesc *ptdesc;
	unsigned long mask, slot, addr;

	spin_lock(&pte_pack_lock);
	if (!list_empty(free_list)) {
		ptdesc = list_first_entry(free_list, struct ptdesc, pt_list);
		mask = ptdesc->pt_index;
		slot = __ffs(mask);
		mask &= ~(1UL << slot);
		ptdesc->pt_index = mask;
		if (mask == 0)
			list_del_init(&ptdesc->pt_list);
		spin_unlock(&pte_pack_lock);

		addr = (unsigned long)ptdesc_address(ptdesc) +
		       slot * PTE_TABLE_SIZE;
		memset((void *)addr, 0, PTE_TABLE_SIZE);
		return (pte_t *)addr;
	}
	spin_unlock(&pte_pack_lock);

	/* Need a fresh kernel page; allocate without holding the lock. */
	ptdesc = pagetable_alloc(gfp, 0);
	if (!ptdesc)
		return NULL;
	if (!kernel) {
		if (!pagetable_pte_ctor(mm, ptdesc)) {
			pagetable_free(ptdesc);
			return NULL;
		}
	} else {
		ptdesc_set_kernel(ptdesc);
	}

	spin_lock(&pte_pack_lock);
	/*
	 * Mark sub-table 0 as in use (returned to caller) and the rest as
	 * free.  At PTE_PER_PAGE=1 there is exactly one sub-table per
	 * ptdesc, so PTE_MARK_FREE & ~1UL == 0 and we skip the free list
	 * entirely (degenerates to the generic allocator).
	 */
	mask = PTE_MARK_FREE & ~1UL;
	ptdesc->pt_index = mask;
	if (mask)
		list_add(&ptdesc->pt_list, free_list);
	spin_unlock(&pte_pack_lock);

	addr = (unsigned long)ptdesc_address(ptdesc);
	/* pagetable_alloc() does not zero; ptdesc->pt_index union shared
	 * with raw pages would be cleared too.  But we just set pt_index
	 * above, so zero only the user-visible PTE area. */
	memset((void *)addr, 0, PTE_TABLE_SIZE);
	return (pte_t *)addr;
}

static void pte_free_packed(struct mm_struct *mm, pte_t *pte,
			    struct list_head *free_list,
			    bool kernel)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pte);
	unsigned long pte_addr = (unsigned long)pte;
	unsigned long bit = 1UL << ptdesc_sub_index(pte_addr);
	unsigned long mask;
	bool was_full, all_free;

	spin_lock(&pte_pack_lock);
	mask = ptdesc->pt_index;
	WARN_ON_ONCE(mask & bit); /* double free of a sub-table */
	was_full = (mask == 0);
	mask |= bit;
	ptdesc->pt_index = mask;
	all_free = (mask == PTE_MARK_FREE);

	if (all_free) {
		if (!was_full)
			list_del_init(&ptdesc->pt_list);
		spin_unlock(&pte_pack_lock);
		if (kernel)
			pagetable_free(ptdesc);
		else
			pagetable_dtor_free(ptdesc);
		return;
	}

	if (was_full)
		list_add(&ptdesc->pt_list, free_list);
	spin_unlock(&pte_pack_lock);
}

pte_t *pte_alloc_one_kernel(struct mm_struct *mm)
{
	return pte_alloc_packed(mm, &pte_kernel_free_list,
				GFP_PGTABLE_KERNEL, true);
}

pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	return pte_alloc_packed(mm, &pte_user_free_list,
				GFP_PGTABLE_USER, false);
}

void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
	pte_free_packed(mm, pte, &pte_kernel_free_list, true);
}

void pte_free(struct mm_struct *mm, pgtable_t pte)
{
	pte_free_packed(mm, pte, &pte_user_free_list, false);
}
