/*
 * include/asm-xtensa/highmem.h
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file "COPYING" in the main directory of
 * this archive for more details.
 *
 * Copyright (C) 2003 - 2005 Tensilica Inc.
 * Copyright (C) 2014 Cadence Design Systems Inc.
 */

#ifndef _XTENSA_HIGHMEM_H
#define _XTENSA_HIGHMEM_H

#ifdef CONFIG_HIGHMEM
#include <linux/wait.h>
#include <linux/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/fixmap.h>

#define PKMAP_BASE		((FIXADDR_START -			\
				  (LAST_PKMAP + 1) * PAGE_SIZE) & PMD_MASK)
#define LAST_PKMAP		(PTRS_PER_PTE * DCACHE_N_COLORS)
#define LAST_PKMAP_MASK		(LAST_PKMAP - 1)
#define PKMAP_NR(virt)		(((virt) - PKMAP_BASE) >> PAGE_SHIFT)
#define PKMAP_ADDR(nr)		(PKMAP_BASE + ((nr) << PAGE_SHIFT))

#define kmap_prot		PAGE_KERNEL_EXEC

#if DCACHE_WAY_SIZE > PAGE_SIZE
#define get_pkmap_color get_pkmap_color
static inline int get_pkmap_color(const struct page *page)
{
	return DCACHE_ALIAS(page_to_phys(page));
}

extern unsigned int last_pkmap_nr_arr[];

static inline unsigned int get_next_pkmap_nr(unsigned int color)
{
	last_pkmap_nr_arr[color] =
		(last_pkmap_nr_arr[color] + DCACHE_N_COLORS) & LAST_PKMAP_MASK;
	return last_pkmap_nr_arr[color] + color;
}

static inline int no_more_pkmaps(unsigned int pkmap_nr, unsigned int color)
{
	return pkmap_nr < DCACHE_N_COLORS;
}

static inline int get_pkmap_entries_count(unsigned int color)
{
	return LAST_PKMAP / DCACHE_N_COLORS;
}

extern wait_queue_head_t pkmap_map_wait_arr[];

static inline wait_queue_head_t *get_pkmap_wait_queue_head(unsigned int color)
{
	return pkmap_map_wait_arr + color;
}

enum fixed_addresses kmap_local_map_idx(int type, unsigned long pfn);
#define arch_kmap_local_map_idx		kmap_local_map_idx

enum fixed_addresses kmap_local_unmap_idx(int type, unsigned long addr);
#define arch_kmap_local_unmap_idx	kmap_local_unmap_idx

#elif defined(CONFIG_PAGE_MMUSHIFT) && CONFIG_PAGE_MMUSHIFT > 0

/*
 * Page clustering without dcache aliasing: a kmap covers a whole PAGE, but the
 * fixmap is indexed in MMUPAGE units, so each logical kmap slot consumes
 * PAGE_MMUCOUNT consecutive fixmap indices.  Scale the generic slot index
 * (idx + KM_MAX_IDX * cpu) accordingly so consecutive kmaps are PAGE-spaced and
 * arch_kmap_local_set_pte's cluster of sub-PTEs fits exactly in one slot.
 */
#include <linux/smp.h>

static inline int arch_kmap_local_map_idx(int idx, unsigned long pfn)
{
	return (idx + KM_MAX_IDX * smp_processor_id()) * PAGE_MMUCOUNT;
}
#define arch_kmap_local_map_idx		arch_kmap_local_map_idx

static inline int arch_kmap_local_unmap_idx(int idx, unsigned long vaddr)
{
	return (idx + KM_MAX_IDX * smp_processor_id()) * PAGE_MMUCOUNT;
}
#define arch_kmap_local_unmap_idx	arch_kmap_local_unmap_idx

#endif

extern pte_t *pkmap_page_table;

static inline void flush_cache_kmaps(void)
{
	flush_cache_all();
}

/*
 * A kmap slot covers one PAGE.  Under page clustering (PAGE_MMUSHIFT > 0) a PAGE
 * is PAGE_MMUCOUNT MMUPAGEs, each needing its own hardware PTE, so install the
 * whole cluster of sub-PTEs (the leaf PTEs are MMUPAGE-granular and contiguous).
 * set_ptes() writes PAGE_MMUCOUNT entries advancing the physical address by
 * MMUPAGE_SIZE each; this is a single set_pte() when PAGE_MMUSHIFT == 0.
 */
#define arch_kmap_local_set_pte(mm, vaddr, ptep, ptev)	\
	set_ptes(mm, vaddr, ptep, ptev, PAGE_MMUCOUNT)

#define arch_kmap_local_post_unmap(vaddr)	\
	local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE)

void kmap_init(void);

#endif /* CONFIG_HIGHMEM */
#endif
