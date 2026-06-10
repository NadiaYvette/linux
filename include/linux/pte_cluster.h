/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PTE_CLUSTER_H
#define _LINUX_PTE_CLUSTER_H

/*
 * include/linux/pte_cluster.h
 *
 * Translated from include/linux/folio.h by Hugh Dickins hugh@veritas.com
 * 31may01 to modern (6.x) kernel APIs.
 *
 * Original naming used "folio" which now conflicts with struct folio
 * (compound page abstraction, 5.16+). Renamed to "pte_cluster".
 *
 * This header is intended for inclusion in mm/memory.c alone.
 * It provides functions for the fault handlers do_fault(),
 * do_anonymous_page(), do_swap_page() and do_wp_page().
 *
 * On a standard system (PAGE_MMUSHIFT == 0), these functions are
 * trivial.  On a system with page clustering (PAGE_MMUSHIFT != 0),
 * they manage the awkwardness of presenting small MMUPAGE_SIZE pages
 * to user programs from a kernel pool of large PAGE_SIZE pages.
 *
 * Shared file mappings present little problem, but without this
 * treatment, private mappings might quickly degenerate into needing
 * one PAGE_SIZE page to support each MMUPAGE_SIZE mapping.
 */

#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/highmem.h>

#if PAGE_MMUSHIFT

/*
 * Test whether pte2 indicates the same page as pte1.
 */
static inline int pte_cluster_match(pte_t *pte1, pte_t *pte2)
{
	if (pte_none(*pte1))
		return pte_none(*pte2);
	if (pte_present(*pte1)) {
		if (!pte_present(*pte2))
			return 0;
		return pte_page(*pte2) == pte_page(*pte1);
	}
	if (pte_none(*pte2) || pte_present(*pte2))
		return 0;
	return __pte_to_swp_entry(*pte2).val == __pte_to_swp_entry(*pte1).val;
}

/*
 * Test whether nearby vma2 could ever share a private page with vma1.
 */
static inline int vma_neighbourly(struct vm_area_struct *vma1,
				  struct vm_area_struct *vma2)
{
	if ((vma1->vm_flags | vma2->vm_flags) & VM_MAYSHARE)
		return 0;
	if ((((vma1->vm_start - vma2->vm_start) >> MMUPAGE_SHIFT) -
	      (vma1->vm_pgoff - vma2->vm_pgoff)) & (PAGE_MMUCOUNT - 1))
		return 0;
	return 1;
}

/*
 * Prepare pte_cluster of page table pointers for the fault handlers.
 *
 * Walks the page table to fill in an array of PTE pointers for all
 * MMU pages within the kernel page containing 'address'.  Entries
 * that fall outside the VMA (or outside the page table) are set to
 * NULL.  When 'wide' is set, neighbouring VMAs that could share a
 * private page are included.
 *
 * Modern port notes:
 * - Uses 5-level page tables (pgd -> p4d -> pud -> pmd -> pte)
 * - Uses pte_offset_map() / pte_unmap() instead of raw pte_offset()
 * - Uses find_vma() and VMA iteration via vma_next()
 *
 * Returns nonzero if the page_table_lock was not held throughout
 * (requiring a re-call after re-acquiring the lock).
 */
static __maybe_unused int prepare_pte_cluster(pte_t *cluster[],
	struct vm_area_struct *vma,
	unsigned long address, pte_t *ptep, int wide)
{
	struct vm_area_struct *vmp;
	unsigned long suboffset;
	unsigned long base, addr;
	int subnr, ptenr;
	int j, limit;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *mapped_pte;
	int reprep = 0;

	suboffset = vma_suboffset(vma, address);
	base = (address - suboffset) & MMUPAGE_MASK;
	subnr = suboffset >> MMUPAGE_SHIFT;
	ptenr = (address & ~PMD_MASK) >> MMUPAGE_SHIFT;

	/* First approximation: set full vector of probable pteps */
	ptep -= subnr;
	for (j = 0; j < PAGE_MMUCOUNT; j++)
		cluster[j] = ptep + j;
	j = 0;

	/* Second approximation: wipe pteps which don't belong to vma;
	 * but if wide, include neighbouring vmas perhaps sharing page.
	 */
	addr = base;
	if (addr > TASK_SIZE) {	/* wrapped */
		for (; addr > TASK_SIZE; addr += MMUPAGE_SIZE, j++)
			cluster[j] = NULL;
	}
	if (addr < vma->vm_start) {
		if (wide) {
			for (vmp = find_vma(vma->vm_mm, addr);
			     vmp && vmp != vma;
			     vmp = find_vma(vma->vm_mm, addr)) {
				for (; addr < vmp->vm_start;
				     addr += MMUPAGE_SIZE, j++)
					cluster[j] = NULL;
				if (vma_neighbourly(vma, vmp)) {
					j += (min(vmp->vm_end,
						  vma->vm_start) - addr)
						>> MMUPAGE_SHIFT;
					addr = min(vmp->vm_end, vma->vm_start);
				} else {
					addr = vmp->vm_end;
				}
				if (addr >= vma->vm_start)
					break;
			}
		}
		for (; addr < vma->vm_start; addr += MMUPAGE_SIZE, j++)
			cluster[j] = NULL;
	}
	if (vma->vm_end < base + PAGE_SIZE) {
		j = (vma->vm_end - base) >> MMUPAGE_SHIFT;
		if (wide) {
			addr = vma->vm_end;
			vmp = find_vma(vma->vm_mm, addr);
			while (vmp && vmp->vm_start < base + PAGE_SIZE) {
				for (; addr < vmp->vm_start;
				     addr += MMUPAGE_SIZE, j++)
					cluster[j] = NULL;
				if (vma_neighbourly(vma, vmp)) {
					unsigned long end = min(vmp->vm_end,
							base + PAGE_SIZE);
					j += (end - addr) >> MMUPAGE_SHIFT;
					addr = end;
				} else {
					addr = vmp->vm_end;
				}
				if (addr >= base + PAGE_SIZE)
					break;
				vmp = find_vma(vma->vm_mm, addr);
			}
		}
		for (; j < PAGE_MMUCOUNT; j++)
			cluster[j] = NULL;
	}

	/* Third approximation: fix pteps to page table below or above.
	 *
	 * If the kernel page spans a page table boundary (the cluster
	 * entries straddle two different PTE pages), we need to walk
	 * the page tables to find the other PTE page.
	 */
	if (subnr > ptenr) {
		limit = subnr - ptenr;
		for (j = 0; !cluster[j]; j++)
			;
		if (j < limit) {
			mapped_pte = NULL;
			pgd = pgd_offset(vma->vm_mm, base);
			if (!pgd_none(*pgd) && !pgd_bad(*pgd)) {
				p4d = p4d_offset(pgd, base);
				if (!p4d_none(*p4d) && !p4d_bad(*p4d)) {
					pud = pud_offset(p4d, base);
					if (!pud_none(*pud) && !pud_bad(*pud)) {
						pmd = pmd_offset(pud, base);
						if (!pmd_none(*pmd) &&
						    !pmd_bad(*pmd))
							mapped_pte = pte_offset_map(pmd, base);
					}
				}
			}
			if (mapped_pte) {
				for (; j < limit; j++) {
					if (cluster[j])
						cluster[j] = mapped_pte + j;
				}
				pte_unmap(mapped_pte);
			} else {
				for (; j < limit; j++)
					cluster[j] = NULL;
				reprep = 1;
			}
		}
	}
	if (ptenr > subnr + PTRS_PER_PTE - PAGE_MMUCOUNT) {
		j = subnr + PTRS_PER_PTE - ptenr;
		for (limit = PAGE_MMUCOUNT; !cluster[limit-1]; limit--)
			;
		if (j < limit) {
			mapped_pte = NULL;
			base += PAGE_SIZE;
			pgd = pgd_offset(vma->vm_mm, base);
			if (!pgd_none(*pgd) && !pgd_bad(*pgd)) {
				p4d = p4d_offset(pgd, base);
				if (!p4d_none(*p4d) && !p4d_bad(*p4d)) {
					pud = pud_offset(p4d, base);
					if (!pud_none(*pud) && !pud_bad(*pud)) {
						pmd = pmd_offset(pud, base);
						if (!pmd_none(*pmd) &&
						    !pmd_bad(*pmd))
							mapped_pte = pte_offset_map(pmd, base);
					}
				}
			}
			if (mapped_pte) {
				mapped_pte -= PAGE_MMUCOUNT;
				for (; j < limit; j++) {
					if (cluster[j])
						cluster[j] = mapped_pte + j;
				}
				pte_unmap(mapped_pte + PAGE_MMUCOUNT);
			} else {
				for (; j < limit; j++)
					cluster[j] = NULL;
				reprep = 1;
			}
		}
	}
	return reprep;	/* needs recall if page_table_lock dropped */
}

/*
 * Check if the wide cluster already has a private page allocated to it.
 *
 * This examines all the PTE entries in the cluster to see if they
 * already reference a private page (one not shared with other processes).
 * If found, the existing page can be reused instead of allocating a new one.
 *
 * swap_page: the page being swapped in (for do_swap_page), or NULL.
 */
static __maybe_unused struct page *private_cluster_page(pte_t *cluster[],
					 struct page *swap_page)
{
	struct page *page;
	struct folio *folio;
	swp_entry_t entry;
	pte_t swap_pte;
	int fcount, pcount, scount, tcount;
	int i, j;

	for (j = PAGE_MMUCOUNT - 1; !cluster[j]; j--)
		;
	fcount = j + 1;

	/*
	 * The easiest way to handle the do_swap_page() case is
	 * to make up one extra element on the end of the cluster:
	 * typically all the cluster entries will be swapped out,
	 * and we need one present page to make sense of them.
	 */
	if (swap_page) {
		swap_pte = mk_pte(swap_page, PAGE_KERNEL);
		cluster[fcount] = &swap_pte;
		fcount++;
	}

	j = 0;
	while (j < fcount) {
		if (!cluster[j] || !pte_present(*cluster[j])) {
			j++;
			continue;
		}
		tcount = 1;
		page = pte_page(*cluster[j]);
		while (++j < fcount) {
			if (!cluster[j] || !pte_present(*cluster[j]))
				continue;
			if (pte_page(*cluster[j]) != page)
				break;
			tcount++;
		}
		if (PageReserved(page))
			continue;
		folio = page_folio(page);
		if (folio_test_swapcache(folio)) {
			if (page != swap_page) {
				if (!folio_trylock(folio))
					continue;
				if (!folio_test_swapcache(folio)) {
					folio_unlock(folio);
					continue;
				}
			}
			entry = folio->swap;
			pcount = page_count(page) - 1;	/* omit swap cache */
			scount = swp_swapcount(entry) - 1; /* omit swap cache */
			if (page != swap_page)
				folio_unlock(folio);
			if (pcount + scount > fcount)
				continue;
		} else {
			if (folio->mapping)
				continue;
			pcount = page_count(page);
			scount = 0;
		}
		pcount -= tcount;
		if (j + pcount > fcount)
			continue;
		for (i = j + 1; pcount && i < fcount; i++) {
			if (!cluster[i] || !pte_present(*cluster[i]))
				continue;
			if (pte_page(*cluster[i]) == page)
				pcount--;
		}
		if (pcount)
			continue;
		for (i = 0; scount && i < fcount; i++) {
			if (!cluster[i] || pte_present(*cluster[i]))
				continue;
			if (softleaf_from_pte(*cluster[i]).val == entry.val)
				scount--;
		}
		if (scount)
			continue;
		return page;
	}
	return NULL;
}

/*
 * Replace page just allocated by private cluster page if it has one.
 */
static inline struct page *private_cluster_page_xchg(pte_t *cluster[],
						     struct page *new_page)
{
	struct page *cluster_page = private_cluster_page(cluster, NULL);

	if (!cluster_page)
		return new_page;
	put_page(new_page);
	get_page(cluster_page);
	return cluster_page;
}

/*
 * Limit cluster to page table entries of this vma matching this *ptep.
 */
static __maybe_unused void restrict_pte_cluster(pte_t *cluster[],
	struct vm_area_struct *vma,
	unsigned long address, pte_t *ptep)
{
	unsigned long addr;
	int j;

	addr = address - vma_suboffset(vma, address);
	for (j = 0; j < PAGE_MMUCOUNT; j++, addr += MMUPAGE_SIZE) {
		if (!cluster[j])
			continue;
		if (addr < vma->vm_start || addr >= vma->vm_end ||
		    !pte_cluster_match(cluster[j], ptep))
			cluster[j] = NULL;
	}
}

/*
 * Copy (or clear) cluster of mmupages from src_page to dst_page.
 *
 * Uses kmap_local_page() (modern replacement for kmap/kunmap).
 * For the zero page case, uses memset to clear.
 */
static __maybe_unused void copy_pte_cluster(pte_t *cluster[],
	struct page *dst_page,
	struct page *src_page, unsigned long address)
{
	char *src, *dst;
	unsigned int size;
	unsigned int offset = 0;
	int j = 0;

	dst = kmap_local_page(dst_page);
	src = (src_page != ZERO_PAGE(address)) ?
		kmap_local_page(src_page) : NULL;

	while (j < PAGE_MMUCOUNT) {
		if (!cluster[j]) {
			offset += MMUPAGE_SIZE;
			j++;
			continue;
		}
		size = MMUPAGE_SIZE;
		while (++j < PAGE_MMUCOUNT) {
			if (!cluster[j])
				break;
			size += MMUPAGE_SIZE;
		}
		if (src)
			memcpy(dst + offset, src + offset, size);
		else
			memset(dst + offset, 0, size);
		offset += size;
	}
	if (src)
		kunmap_local(src);
	kunmap_local(dst);
}

/*
 * Update page table entries of the cluster, counting how many done.
 */
static inline unsigned long set_pte_cluster(pte_t *cluster[], pte_t pte)
{
	unsigned long offset = 0;
	unsigned long rss = 0;
	int j;

	for (j = 0; j < PAGE_MMUCOUNT; j++, offset += MMUPAGE_SIZE) {
		if (!cluster[j])
			continue;
		set_ptes(NULL, 0, cluster[j], pte_mksub(pte, offset), 1);
		rss++;
	}
	return rss;
}

/*
 * Flush TLB entries for the cluster (if ptes were present before).
 */
static inline void flush_pte_cluster(pte_t *cluster[],
	struct vm_area_struct *vma, unsigned long address)
{
	unsigned long start, end;
	int j;

	start = (address - vma_suboffset(vma, address)) & MMUPAGE_MASK;
	end = start + PAGE_SIZE;
	for (j = 0; !cluster[j]; j++)
		start += MMUPAGE_SIZE;
	for (j = PAGE_MMUCOUNT - 1; !cluster[j]; j--)
		end -= MMUPAGE_SIZE;
	flush_tlb_range(vma, start, end);
}

#define adjust_page_ref_count(page, extra) \
	page_ref_add(page, extra)

#else  /* PAGE_MMUSHIFT 0 */

static inline int prepare_pte_cluster(pte_t *cluster[],
	struct vm_area_struct *vma,
	unsigned long address, pte_t *ptep, int wide)
{
	cluster[0] = ptep;
	return 0;
}

/*
 * Calling convention different if !PAGE_MMUSHIFT: page always passed in.
 *
 * Check if this is a private page that can be reused for COW.
 */
static inline struct page *private_cluster_page(pte_t *cluster[],
						struct page *page)
{
	struct folio *folio = page_folio(page);
	int doing_wp = pte_present(*cluster[0]);
	int count;

	if (PageReserved(page))
		return NULL;
	if (folio_test_swapcache(folio)) {
		if (doing_wp) {
			if (!folio_trylock(folio))
				return NULL;
			if (!folio_test_swapcache(folio)) {
				folio_unlock(folio);
				return NULL;
			}
		}
		count = page_count(page) + swp_swapcount(folio->swap) - 3;
		if (doing_wp)
			folio_unlock(folio);
		else
			count--;	/* swap not yet freed */
	} else {
		count = page_count(page) - 1;
	}
	return count ? NULL : page;
}

#define private_cluster_page_xchg(cluster, new_page) \
	(new_page)

#define restrict_pte_cluster(cluster, vma, address, ptep) \
	do {} while (0)

static inline void copy_pte_cluster(pte_t *cluster[], struct page *dst_page,
	struct page *src_page, unsigned long address)
{
	if (src_page == ZERO_PAGE(address)) {
		clear_highpage(dst_page);
	} else {
		copy_highpage(dst_page, src_page);
	}
}

static inline unsigned long set_pte_cluster(pte_t *cluster[], pte_t pte)
{
	set_ptes(NULL, 0, cluster[0], pte, 1);
	return 1;
}

#define flush_pte_cluster(cluster, vma, address) \
	flush_tlb_page((vma), (address))

#define adjust_page_ref_count(page, extra) \
	do {} while (0)

#endif /* PAGE_MMUSHIFT 0 */

#endif /* _LINUX_PTE_CLUSTER_H */
