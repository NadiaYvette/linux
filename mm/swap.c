// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/admin-guide/sysctl/vm.rst.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/folio_batch.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mm_inline.h>
#include <linux/percpu_counter.h>
#include <linux/memremap.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/backing-dev.h>
#include <linux/memcontrol.h>
#include <linux/gfp.h>
#include <linux/uio.h>
#include <linux/hugetlb.h>
#include <linux/page_idle.h>
#include <linux/local_lock.h>
#include <linux/buffer_head.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/pagemap.h>

/* How many pages do we try to swap or page in/out together? As a power of 2 */
int page_cluster;
static const int page_cluster_max = 31;

struct cpu_fbatches {
	/*
	 * The following folio batches are grouped together because they are protected
	 * by disabling preemption (and interrupts remain enabled).
	 */
	local_lock_t lock;
	struct folio_batch lru_add;
	struct folio_batch lru_deactivate_file;
	struct folio_batch lru_deactivate;
	struct folio_batch lru_lazyfree;
#ifdef CONFIG_SMP
	struct folio_batch lru_activate;
#endif
	/* Protecting the following batches which require disabling interrupts */
	local_lock_t lock_irq;
	struct folio_batch lru_move_tail;
};

static DEFINE_PER_CPU(struct cpu_fbatches, cpu_fbatches) = {
	.lock = INIT_LOCAL_LOCK(lock),
	.lock_irq = INIT_LOCAL_LOCK(lock_irq),
};

static void __page_cache_release(struct folio *folio, struct lruvec **lruvecp,
		unsigned long *flagsp)
{
	if (folio_test_lru(folio)) {
		folio_lruvec_relock_irqsave(folio, lruvecp, flagsp);
		lruvec_del_folio(*lruvecp, folio);
		__folio_clear_lru_flags(folio);
	}
}

/*
 * This path almost never happens for VM activity - pages are normally freed
 * in batches.  But it gets used by networking - and for compound pages.
 */
static void page_cache_release(struct folio *folio)
{
	struct lruvec *lruvec = NULL;
	unsigned long flags;

	__page_cache_release(folio, &lruvec, &flags);
	if (lruvec)
		lruvec_unlock_irqrestore(lruvec, flags);
}

void __folio_put(struct folio *folio)
{
	if (unlikely(folio_is_zone_device(folio))) {
		free_zone_device_folio(folio);
		return;
	}

	if (folio_test_hugetlb(folio)) {
		free_huge_folio(folio);
		return;
	}

#if PAGE_MMUSHIFT
	/*
	 * #143 CACHE-FLOOR guard at the single-folio put chokepoint (Tessera
	 * FileCacheRef.rehold_floorOk) -- the twin of the folios_put_refs guard,
	 * covering the path the batch guard misses.  __folio_put runs at refcount 0;
	 * a still-CACHED file folio here (mapping != NULL, !anon, !swapbacked) means
	 * its cache ref was over-dropped -- a double-drop racing the gather's
	 * deferred drop (reinc #38 CACHEFLOOR: nr_refs=1, refcount 1->0 on a
	 * cache-only file folio).  Freeing it reuses a shared page-cache page (e.g. a
	 * libcef.so code page) -> userspace wrong-data (Electron int3 CHECK-fail,
	 * segfaults).  Enforce cachedPinned: re-hold to the cache floor and skip the
	 * free; the ratelimited PGCL143-CACHEFLOOR + dump_stack NAMES the racing put
	 * (the shadow that pins the double-drop's source).  A legit eviction clears
	 * mapping first (delete_from_page_cache), so this never fires on a correct
	 * free -- leak-on-race beats shared-page corruption.
	 */
	if (unlikely(folio->mapping && !folio_test_anon(folio))) {
		long cfloor = folio_nr_pages(folio);
		static DEFINE_RATELIMIT_STATE(rs_cf2, HZ, 4);

		if (__ratelimit(&rs_cf2)) {
			pr_warn("PGCL143-CACHEFLOOR: __folio_put freeing cached file/shmem folio pfn=%#lx refcount=%d floor=%ld (mapping=%px); over-drop site:\n",
				folio_pfn(folio), folio_ref_count(folio), cfloor,
				folio->mapping);
			dump_stack();
		}
		folio_ref_add(folio, cfloor);
		return;
	}
#endif

	page_cache_release(folio);
	folio_unqueue_deferred_split(folio);
	mem_cgroup_uncharge(folio);
	free_frozen_pages(&folio->page, folio_order(folio));
}
EXPORT_SYMBOL(__folio_put);

typedef void (*move_fn_t)(struct lruvec *lruvec, struct folio *folio);

static void lru_add(struct lruvec *lruvec, struct folio *folio)
{
	int was_unevictable = folio_test_clear_unevictable(folio);
	long nr_pages = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/*
	 * Is an smp_mb__after_atomic() still required here, before
	 * folio_evictable() tests the mlocked flag, to rule out the possibility
	 * of stranding an evictable folio on an unevictable LRU?  I think
	 * not, because __munlock_folio() only clears the mlocked flag
	 * while the LRU lock is held.
	 *
	 * (That is not true of __page_cache_release(), and not necessarily
	 * true of folios_put(): but those only clear the mlocked flag after
	 * folio_put_testzero() has excluded any other users of the folio.)
	 */
	if (folio_evictable(folio)) {
		if (was_unevictable)
			__count_vm_events(UNEVICTABLE_PGRESCUED, nr_pages);
	} else {
		folio_clear_active(folio);
		folio_set_unevictable(folio);
		/*
		 * folio->mlock_count = !!folio_test_mlocked(folio)?
		 * But that leaves __mlock_folio() in doubt whether another
		 * actor has already counted the mlock or not.  Err on the
		 * safe side, underestimate, let page reclaim fix it, rather
		 * than leaving a page on the unevictable LRU indefinitely.
		 */
		folio->mlock_count = 0;
		if (!was_unevictable)
			__count_vm_events(UNEVICTABLE_PGCULLED, nr_pages);
	}

	lruvec_add_folio(lruvec, folio);
	trace_mm_lru_insertion(folio);
}

static void folio_batch_move_lru(struct folio_batch *fbatch, move_fn_t move_fn)
{
	int i;
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;

	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];

		/* block memcg migration while the folio moves between lru */
		if (move_fn != lru_add && !folio_test_clear_lru(folio))
			continue;

		folio_lruvec_relock_irqsave(folio, &lruvec, &flags);
		move_fn(lruvec, folio);

		folio_set_lru(folio);
	}

	if (lruvec)
		lruvec_unlock_irqrestore(lruvec, flags);
	folios_put(fbatch);
}

static void __folio_batch_add_and_move(struct folio_batch __percpu *fbatch,
		struct folio *folio, move_fn_t move_fn, bool disable_irq)
{
	unsigned long flags;

	folio_get(folio);

	if (disable_irq)
		local_lock_irqsave(&cpu_fbatches.lock_irq, flags);
	else
		local_lock(&cpu_fbatches.lock);

	if (!folio_batch_add(this_cpu_ptr(fbatch), folio) ||
			!folio_may_be_lru_cached(folio) || lru_cache_disabled())
		folio_batch_move_lru(this_cpu_ptr(fbatch), move_fn);

	if (disable_irq)
		local_unlock_irqrestore(&cpu_fbatches.lock_irq, flags);
	else
		local_unlock(&cpu_fbatches.lock);
}

#define folio_batch_add_and_move(folio, op)		\
	__folio_batch_add_and_move(			\
		&cpu_fbatches.op,			\
		folio,					\
		op,					\
		offsetof(struct cpu_fbatches, op) >=	\
		offsetof(struct cpu_fbatches, lock_irq)	\
	)

static void lru_move_tail(struct lruvec *lruvec, struct folio *folio)
{
	if (folio_test_unevictable(folio))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	lruvec_add_folio_tail(lruvec, folio);
	__count_vm_events(PGROTATED, folio_nr_pages(folio));
}

/*
 * Writeback is about to end against a folio which has been marked for
 * immediate reclaim.  If it still appears to be reclaimable, move it
 * to the tail of the inactive list.
 *
 * folio_rotate_reclaimable() must disable IRQs, to prevent nasty races.
 */
void folio_rotate_reclaimable(struct folio *folio)
{
	if (folio_test_locked(folio) || folio_test_dirty(folio) ||
	    folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;

	folio_batch_add_and_move(folio, lru_move_tail);
}

void lru_note_cost_unlock_irq(struct lruvec *lruvec, bool file,
		unsigned int nr_io, unsigned int nr_rotated)
		__releases(lruvec->lru_lock)
		__releases(rcu)
{
	unsigned long cost;

	/*
	 * Reflect the relative cost of incurring IO and spending CPU
	 * time on rotations. This doesn't attempt to make a precise
	 * comparison, it just says: if reloads are about comparable
	 * between the LRU lists, or rotations are overwhelmingly
	 * different between them, adjust scan balance for CPU work.
	 */
	cost = nr_io * SWAP_CLUSTER_MAX + nr_rotated;
	if (!cost) {
		spin_unlock_irq(&lruvec->lru_lock);
		rcu_read_unlock();
		return;
	}

	for (;;) {
		unsigned long lrusize;

		/* Record cost event */
		if (file)
			lruvec->file_cost += cost;
		else
			lruvec->anon_cost += cost;

		/*
		 * Decay previous events
		 *
		 * Because workloads change over time (and to avoid
		 * overflow) we keep these statistics as a floating
		 * average, which ends up weighing recent refaults
		 * more than old ones.
		 */
		lrusize = lruvec_page_state(lruvec, NR_INACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_ACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_INACTIVE_FILE) +
			  lruvec_page_state(lruvec, NR_ACTIVE_FILE);

		if (lruvec->file_cost + lruvec->anon_cost > lrusize / 4) {
			lruvec->file_cost /= 2;
			lruvec->anon_cost /= 2;
		}

		spin_unlock_irq(&lruvec->lru_lock);
		lruvec = parent_lruvec(lruvec);
		if (!lruvec) {
			rcu_read_unlock();
			break;
		}
		spin_lock_irq(&lruvec->lru_lock);
	}
}

void lru_note_cost_refault(struct folio *folio)
{
	struct lruvec *lruvec;

	lruvec = folio_lruvec_lock_irq(folio);
	lru_note_cost_unlock_irq(lruvec, folio_is_file_lru(folio),
				folio_nr_pages(folio), 0);
}

static void lru_activate(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_active(folio) || folio_test_unevictable(folio))
		return;


	lruvec_del_folio(lruvec, folio);
	folio_set_active(folio);
	lruvec_add_folio(lruvec, folio);
	trace_mm_lru_activate(folio);

	__count_vm_events(PGACTIVATE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGACTIVATE, nr_pages);
}

#ifdef CONFIG_SMP
static void folio_activate_drain(int cpu)
{
	struct folio_batch *fbatch = &per_cpu(cpu_fbatches.lru_activate, cpu);

	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_activate);
}

void folio_activate(struct folio *folio)
{
	if (folio_test_active(folio) || folio_test_unevictable(folio) ||
	    !folio_test_lru(folio))
		return;

	folio_batch_add_and_move(folio, lru_activate);
}

#else
static inline void folio_activate_drain(int cpu)
{
}

void folio_activate(struct folio *folio)
{
	struct lruvec *lruvec;

	if (!folio_test_clear_lru(folio))
		return;

	lruvec = folio_lruvec_lock_irq(folio);
	lru_activate(lruvec, folio);
	lruvec_unlock_irq(lruvec);
	folio_set_lru(folio);
}
#endif

static void __lru_cache_activate_folio(struct folio *folio)
{
	struct folio_batch *fbatch;
	int i;

	local_lock(&cpu_fbatches.lock);
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_add);

	/*
	 * Search backwards on the optimistic assumption that the folio being
	 * activated has just been added to this batch. Note that only
	 * the local batch is examined as a !LRU folio could be in the
	 * process of being released, reclaimed, migrated or on a remote
	 * batch that is currently being drained. Furthermore, marking
	 * a remote batch's folio active potentially hits a race where
	 * a folio is marked active just after it is added to the inactive
	 * list causing accounting errors and BUG_ON checks to trigger.
	 */
	for (i = folio_batch_count(fbatch) - 1; i >= 0; i--) {
		struct folio *batch_folio = fbatch->folios[i];

		if (batch_folio == folio) {
			folio_set_active(folio);
			break;
		}
	}

	local_unlock(&cpu_fbatches.lock);
}

#ifdef CONFIG_LRU_GEN

static void lru_gen_inc_refs(struct folio *folio)
{
	unsigned long new_flags, old_flags = READ_ONCE(folio->flags.f);

	if (folio_test_unevictable(folio))
		return;

	/* see the comment on LRU_REFS_FLAGS */
	if (!folio_test_referenced(folio)) {
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, BIT(PG_referenced));
		return;
	}

	do {
		if ((old_flags & LRU_REFS_MASK) == LRU_REFS_MASK) {
			if (!folio_test_workingset(folio))
				folio_set_workingset(folio);
			return;
		}

		new_flags = old_flags + BIT(LRU_REFS_PGOFF);
	} while (!try_cmpxchg(&folio->flags.f, &old_flags, new_flags));
}

static bool lru_gen_clear_refs(struct folio *folio)
{
	int gen = folio_lru_gen(folio);
	int type = folio_is_file_lru(folio);
	unsigned long seq;

	if (gen < 0)
		return true;

	set_mask_bits(&folio->flags.f, LRU_REFS_FLAGS | BIT(PG_workingset), 0);

	rcu_read_lock();
	seq = READ_ONCE(folio_lruvec(folio)->lrugen.min_seq[type]);
	rcu_read_unlock();
	/* whether can do without shuffling under the LRU lock */
	return gen == lru_gen_from_seq(seq);
}

#else /* !CONFIG_LRU_GEN */

static void lru_gen_inc_refs(struct folio *folio)
{
}

static bool lru_gen_clear_refs(struct folio *folio)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

/**
 * folio_mark_accessed - Mark a folio as having seen activity.
 * @folio: The folio to mark.
 *
 * This function will perform one of the following transitions:
 *
 * * inactive,unreferenced	->	inactive,referenced
 * * inactive,referenced	->	active,unreferenced
 * * active,unreferenced	->	active,referenced
 *
 * When a newly allocated folio is not yet visible, so safe for non-atomic ops,
 * __folio_set_referenced() may be substituted for folio_mark_accessed().
 */
void folio_mark_accessed(struct folio *folio)
{
	if (folio_test_dropbehind(folio))
		return;
	if (lru_gen_enabled()) {
		lru_gen_inc_refs(folio);
		return;
	}

	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);
	} else if (folio_test_unevictable(folio)) {
		/*
		 * Unevictable pages are on the "LRU_UNEVICTABLE" list. But,
		 * this list is never rotated or maintained, so marking an
		 * unevictable page accessed has no effect.
		 */
	} else if (!folio_test_active(folio)) {
		/*
		 * If the folio is on the LRU, queue it for activation via
		 * cpu_fbatches.lru_activate. Otherwise, assume the folio is in a
		 * folio_batch, mark it active and it'll be moved to the active
		 * LRU on the next drain.
		 */
		if (folio_test_lru(folio))
			folio_activate(folio);
		else
			__lru_cache_activate_folio(folio);
		folio_clear_referenced(folio);
		workingset_activation(folio);
	}
	if (folio_test_idle(folio))
		folio_clear_idle(folio);
}
EXPORT_SYMBOL(folio_mark_accessed);

/**
 * folio_add_lru - Add a folio to an LRU list.
 * @folio: The folio to be added to the LRU.
 *
 * Queue the folio for addition to the LRU. The decision on whether
 * to add the page to the [in]active [file|anon] list is deferred until the
 * folio_batch is drained. This gives a chance for the caller of folio_add_lru()
 * have the folio added to the active list using folio_mark_accessed().
 */
void folio_add_lru(struct folio *folio)
{
	VM_BUG_ON_FOLIO(folio_test_active(folio) &&
			folio_test_unevictable(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/* see the comment in lru_gen_folio_seq() */
	if (lru_gen_enabled() && !folio_test_unevictable(folio) &&
	    lru_gen_in_fault() && !(current->flags & PF_MEMALLOC))
		folio_set_active(folio);

	folio_batch_add_and_move(folio, lru_add);
}
EXPORT_SYMBOL(folio_add_lru);

/**
 * folio_add_lru_vma() - Add a folio to the appropriate LRU list for this VMA.
 * @folio: The folio to be added to the LRU.
 * @vma: VMA in which the folio is mapped.
 *
 * If the VMA is mlocked, @folio is added to the unevictable list.
 * Otherwise, it is treated the same way as folio_add_lru().
 */
void folio_add_lru_vma(struct folio *folio, struct vm_area_struct *vma)
{
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	if (unlikely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) == VM_LOCKED))
		mlock_new_folio(folio);
	else
		folio_add_lru(folio);
}

/*
 * If the folio cannot be invalidated, it is moved to the
 * inactive list to speed up its reclaim.  It is moved to the
 * head of the list, rather than the tail, to give the flusher
 * threads some time to write it out, as this is much more
 * effective than the single-page writeout from reclaim.
 *
 * If the folio isn't mapped and dirty/writeback, the folio
 * could be reclaimed asap using the reclaim flag.
 *
 * 1. active, mapped folio -> none
 * 2. active, dirty/writeback folio -> inactive, head, reclaim
 * 3. inactive, mapped folio -> none
 * 4. inactive, dirty/writeback folio -> inactive, head, reclaim
 * 5. inactive, clean -> inactive, tail
 * 6. Others -> none
 *
 * In 4, it moves to the head of the inactive list so the folio is
 * written out by flusher threads as this is much more efficient
 * than the single-page writeout from reclaim.
 */
static void lru_deactivate_file(struct lruvec *lruvec, struct folio *folio)
{
	bool active = folio_test_active(folio) || lru_gen_enabled();
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_unevictable(folio))
		return;

	/* Some processes are using the folio */
	if (folio_mapped(folio))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	folio_clear_referenced(folio);

	if (folio_test_writeback(folio) || folio_test_dirty(folio)) {
		/*
		 * Setting the reclaim flag could race with
		 * folio_end_writeback() and confuse readahead.  But the
		 * race window is _really_ small and  it's not a critical
		 * problem.
		 */
		lruvec_add_folio(lruvec, folio);
		folio_set_reclaim(folio);
	} else {
		/*
		 * The folio's writeback ended while it was in the batch.
		 * We move that folio to the tail of the inactive list.
		 */
		lruvec_add_folio_tail(lruvec, folio);
		__count_vm_events(PGROTATED, nr_pages);
	}

	if (active) {
		__count_vm_events(PGDEACTIVATE, nr_pages);
		count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE,
				     nr_pages);
	}
}

static void lru_deactivate(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_unevictable(folio) || !(folio_test_active(folio) || lru_gen_enabled()))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	folio_clear_referenced(folio);
	lruvec_add_folio(lruvec, folio);

	__count_vm_events(PGDEACTIVATE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_pages);
}

static void lru_lazyfree(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    folio_test_swapcache(folio) || folio_test_unevictable(folio))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	if (lru_gen_enabled())
		lru_gen_clear_refs(folio);
	else
		folio_clear_referenced(folio);
	/*
	 * Lazyfree folios are clean anonymous folios.  They have
	 * the swapbacked flag cleared, to distinguish them from normal
	 * anonymous folios
	 */
	folio_clear_swapbacked(folio);
	lruvec_add_folio(lruvec, folio);

	__count_vm_events(PGLAZYFREE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGLAZYFREE, nr_pages);
}

/*
 * Drain pages out of the cpu's folio_batch.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
void lru_add_drain_cpu(int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);
	struct folio_batch *fbatch = &fbatches->lru_add;

	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_add);

	fbatch = &fbatches->lru_move_tail;
	/* Disabling interrupts below acts as a compiler barrier. */
	if (data_race(folio_batch_count(fbatch))) {
		unsigned long flags;

		/* No harm done if a racing interrupt already did this */
		local_lock_irqsave(&cpu_fbatches.lock_irq, flags);
		folio_batch_move_lru(fbatch, lru_move_tail);
		local_unlock_irqrestore(&cpu_fbatches.lock_irq, flags);
	}

	fbatch = &fbatches->lru_deactivate_file;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate_file);

	fbatch = &fbatches->lru_deactivate;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate);

	fbatch = &fbatches->lru_lazyfree;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_lazyfree);

	folio_activate_drain(cpu);
}

/**
 * deactivate_file_folio() - Deactivate a file folio.
 * @folio: Folio to deactivate.
 *
 * This function hints to the VM that @folio is a good reclaim candidate,
 * for example if its invalidation fails due to the folio being dirty
 * or under writeback.
 *
 * Context: Caller holds a reference on the folio.
 */
void deactivate_file_folio(struct folio *folio)
{
	/* Deactivating an unevictable folio will not accelerate reclaim */
	if (folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;

	if (lru_gen_enabled() && lru_gen_clear_refs(folio))
		return;

	folio_batch_add_and_move(folio, lru_deactivate_file);
}

/*
 * folio_deactivate - deactivate a folio
 * @folio: folio to deactivate
 *
 * folio_deactivate() moves @folio to the inactive list if @folio was on the
 * active list and was not unevictable. This is done to accelerate the
 * reclaim of @folio.
 */
void folio_deactivate(struct folio *folio)
{
	if (folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;

	if (lru_gen_enabled() ? lru_gen_clear_refs(folio) : !folio_test_active(folio))
		return;

	folio_batch_add_and_move(folio, lru_deactivate);
}

/**
 * folio_mark_lazyfree - make an anon folio lazyfree
 * @folio: folio to deactivate
 *
 * folio_mark_lazyfree() moves @folio to the inactive file list.
 * This is done to accelerate the reclaim of @folio.
 */
void folio_mark_lazyfree(struct folio *folio)
{
	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    !folio_test_lru(folio) ||
	    folio_test_swapcache(folio) || folio_test_unevictable(folio))
		return;

	folio_batch_add_and_move(folio, lru_lazyfree);
}

void lru_add_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

/*
 * It's called from per-cpu workqueue context in SMP case so
 * lru_add_drain_cpu and invalidate_bh_lrus_cpu should run on
 * the same cpu. It shouldn't be a problem in !SMP case since
 * the core is only one and the locks will disable preemption.
 */
static void lru_add_and_bh_lrus_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	invalidate_bh_lrus_cpu();
	mlock_drain_local();
}

void lru_add_drain_cpu_zone(struct zone *zone)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	drain_local_pages(zone);
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

#ifdef CONFIG_SMP

static DEFINE_PER_CPU(struct work_struct, lru_add_drain_work);

static void lru_add_drain_per_cpu(struct work_struct *dummy)
{
	lru_add_and_bh_lrus_drain();
}

static bool cpu_needs_drain(unsigned int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);

	/* Check these in order of likelihood that they're not zero */
	return folio_batch_count(&fbatches->lru_add) ||
		folio_batch_count(&fbatches->lru_move_tail) ||
		folio_batch_count(&fbatches->lru_deactivate_file) ||
		folio_batch_count(&fbatches->lru_deactivate) ||
		folio_batch_count(&fbatches->lru_lazyfree) ||
		folio_batch_count(&fbatches->lru_activate) ||
		need_mlock_drain(cpu) ||
		has_bh_in_lru(cpu, NULL);
}

/*
 * Doesn't need any cpu hotplug locking because we do rely on per-cpu
 * kworkers being shut down before our page_alloc_cpu_dead callback is
 * executed on the offlined cpu.
 * Calling this function with cpu hotplug locks held can actually lead
 * to obscure indirect dependencies via WQ context.
 */
static inline void __lru_add_drain_all(bool force_all_cpus)
{
	/*
	 * lru_drain_gen - Global pages generation number
	 *
	 * (A) Definition: global lru_drain_gen = x implies that all generations
	 *     0 < n <= x are already *scheduled* for draining.
	 *
	 * This is an optimization for the highly-contended use case where a
	 * user space workload keeps constantly generating a flow of pages for
	 * each CPU.
	 */
	static unsigned int lru_drain_gen;
	static struct cpumask has_work;
	static DEFINE_MUTEX(lock);
	unsigned cpu, this_gen;

	/*
	 * Make sure nobody triggers this path before mm_percpu_wq is fully
	 * initialized.
	 */
	if (WARN_ON(!mm_percpu_wq))
		return;

	/*
	 * Guarantee folio_batch counter stores visible by this CPU
	 * are visible to other CPUs before loading the current drain
	 * generation.
	 */
	smp_mb();

	/*
	 * (B) Locally cache global LRU draining generation number
	 *
	 * The read barrier ensures that the counter is loaded before the mutex
	 * is taken. It pairs with smp_mb() inside the mutex critical section
	 * at (D).
	 */
	this_gen = smp_load_acquire(&lru_drain_gen);

	/* It helps everyone if we do our own local drain immediately. */
	lru_add_drain();

	mutex_lock(&lock);

	/*
	 * (C) Exit the draining operation if a newer generation, from another
	 * lru_add_drain_all(), was already scheduled for draining. Check (A).
	 */
	if (unlikely(this_gen != lru_drain_gen && !force_all_cpus))
		goto done;

	/*
	 * (D) Increment global generation number
	 *
	 * Pairs with smp_load_acquire() at (B), outside of the critical
	 * section. Use a full memory barrier to guarantee that the
	 * new global drain generation number is stored before loading
	 * folio_batch counters.
	 *
	 * This pairing must be done here, before the for_each_online_cpu loop
	 * below which drains the page vectors.
	 *
	 * Let x, y, and z represent some system CPU numbers, where x < y < z.
	 * Assume CPU #z is in the middle of the for_each_online_cpu loop
	 * below and has already reached CPU #y's per-cpu data. CPU #x comes
	 * along, adds some pages to its per-cpu vectors, then calls
	 * lru_add_drain_all().
	 *
	 * If the paired barrier is done at any later step, e.g. after the
	 * loop, CPU #x will just exit at (C) and miss flushing out all of its
	 * added pages.
	 */
	WRITE_ONCE(lru_drain_gen, lru_drain_gen + 1);
	smp_mb();

	cpumask_clear(&has_work);
	for_each_online_cpu(cpu) {
		struct work_struct *work = &per_cpu(lru_add_drain_work, cpu);

		if (cpu_needs_drain(cpu)) {
			INIT_WORK(work, lru_add_drain_per_cpu);
			queue_work_on(cpu, mm_percpu_wq, work);
			__cpumask_set_cpu(cpu, &has_work);
		}
	}

	for_each_cpu(cpu, &has_work)
		flush_work(&per_cpu(lru_add_drain_work, cpu));

done:
	mutex_unlock(&lock);
}

void lru_add_drain_all(void)
{
	__lru_add_drain_all(false);
}
#else
void lru_add_drain_all(void)
{
	lru_add_drain();
}
#endif /* CONFIG_SMP */

atomic_t lru_disable_count = ATOMIC_INIT(0);

/*
 * lru_cache_disable() needs to be called before we start compiling
 * a list of folios to be migrated using folio_isolate_lru().
 * It drains folios on LRU cache and then disable on all cpus until
 * lru_cache_enable is called.
 *
 * Must be paired with a call to lru_cache_enable().
 */
void lru_cache_disable(void)
{
	atomic_inc(&lru_disable_count);
	/*
	 * Readers of lru_disable_count are protected by either disabling
	 * preemption or rcu_read_lock:
	 *
	 * preempt_disable, local_irq_disable  [bh_lru_lock()]
	 * rcu_read_lock		       [rt_spin_lock CONFIG_PREEMPT_RT]
	 * preempt_disable		       [local_lock !CONFIG_PREEMPT_RT]
	 *
	 * Since v5.1 kernel, synchronize_rcu() is guaranteed to wait on
	 * preempt_disable() regions of code. So any CPU which sees
	 * lru_disable_count = 0 will have exited the critical
	 * section when synchronize_rcu() returns.
	 */
	synchronize_rcu_expedited();
#ifdef CONFIG_SMP
	__lru_add_drain_all(true);
#else
	lru_add_and_bh_lrus_drain();
#endif
}

/**
 * folios_put_refs - Reduce the reference count on a batch of folios.
 * @folios: The folios.
 * @refs: The number of refs to subtract from each folio.
 *
 * Like folio_put(), but for a batch of folios.  This is more efficient
 * than writing the loop yourself as it will optimise the locks which need
 * to be taken if the folios are freed.  The folios batch is returned
 * empty and ready to be reused for another batch; there is no need
 * to reinitialise it.  If @refs is NULL, we subtract one from each
 * folio refcount.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
void folios_put_refs(struct folio_batch *folios, unsigned int *refs)
{
	int i, j;
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;

	for (i = 0, j = 0; i < folios->nr; i++) {
		struct folio *folio = folios->folios[i];
		unsigned int nr_refs = refs ? refs[i] : 1;

		if (is_huge_zero_folio(folio))
			continue;

		if (folio_is_zone_device(folio)) {
			if (lruvec) {
				lruvec_unlock_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			if (folio_ref_sub_and_test(folio, nr_refs))
				free_zone_device_folio(folio);
			continue;
		}

#if PAGE_MMUSHIFT
		{
			/*
			 * PGCL #143 band-aid (refcount floor) -- twin of the _mapcount
			 * floor in __folio_remove_rmap.  The cross-mm aggregate over-put
			 * drives a shared cluster's _refcount NEGATIVE (refcount:-N ->
			 * "Bad page state" -> panic, the -pgcl4hard2 failure).  Floor the
			 * subtract at 0: an over-put frees the cluster cleanly at 0
			 * instead of underflowing; the excess (never-taken) puts are
			 * dropped (leak-not-corrupt).  A normal put (refs >= nr_refs) is
			 * byte-identical to folio_ref_sub_and_test.
			 */
			int old = folio_ref_count(folio), new_refs;

			do {
				new_refs = old > (int)nr_refs ? old - (int)nr_refs : 0;
			} while (!atomic_try_cmpxchg(&folio->_refcount, &old, new_refs));
			/*
			 * #143 DOUBLE-DROP detector (task #17, the reinc #40 residual): a
			 * file/shmem folio the mmu_gather still OWES a deferred free for
			 * (pgcl143_gather_owes stamp set) is being ref-dropped HERE by a
			 * NON-gather path (!in_gflush).  If this drop takes it to/below the
			 * cache floor -- all mapping refs gone -- the gather's later deferred
			 * drop DOUBLE-FREES it: the page lands on the pcp free-list twice ->
			 * list_del/add corruption, shared-lib page reuse -> Electron int3 /
			 * segfault.  bad_page stays 0 (mapping cleared by then), so only this
			 * names it.  Report the eager-dropper (dump_stack) + the zap that
			 * deferred (gather_ip); WARN-only, the CACHE-FLOOR guard below still
			 * re-holds to prevent the actual double-free.
			 */
			{
				unsigned long ddpfn = folio_pfn(folio);
				unsigned int ddi = pgcl143_pending_idx(ddpfn);

				if (unlikely(new_refs <= (int)folio_nr_pages(folio) &&
					     folio->mapping && !folio_test_anon(folio) &&
					     !this_cpu_read(pgcl143_in_gflush) &&
					     pgcl143_gather_owes[ddi] == ddpfn)) {
					static DEFINE_RATELIMIT_STATE(rs_dd, HZ, 4);

					if (__ratelimit(&rs_dd)) {
						pr_warn("PGCL143-DOUBLEDROP: file/shmem folio pfn=%#lx ref-dropped to %d (floor %ld) by non-gather path while gather owes it (deferred-by=%pS); eager-dropper:\n",
							ddpfn, new_refs,
							folio_nr_pages(folio),
							(void *)pgcl143_gather_ip[ddi]);
						dump_stack();
					}
				}
			}
			/*
			 * #143 CACHE-FLOOR detector + enforcement (Tessera
			 * FileCacheRef.rehold_floorOk / violated_iff_not_floorOk).  A FILE
			 * page-cache folio holds >= folio_nr_pages structural refs while
			 * cached (mapping != NULL): filemap_add_folio takes them, dropped
			 * only by truncate/__remove_mapping AFTER clearing mapping.  reinc
			 * #37 showed the gather / lru_add-drain put dropping such a folio
			 * BELOW that floor WHILE STILL cached -> freed with mapping/private
			 * set -> pcp free-list corruption (__rmqueue_pcplist /
			 * free_frozen_page_commit) -> allocator wedge -> GUI freeze.  Detect
			 * (PGCL143-CACHEFLOOR names the racing put) and ENFORCE cachedPinned:
			 * re-hold to the floor and skip the free.  Anon/swap excluded
			 * (mapping is anon_vma / auto-cleared at free); a legit eviction
			 * clears mapping first, so this never fires on a correct free --
			 * leak-on-race beats corruption.
			 */
			if (unlikely(new_refs < (int)folio_nr_pages(folio) &&
				     folio->mapping && !folio_test_anon(folio))) {
				long cfloor = folio_nr_pages(folio);
				static DEFINE_RATELIMIT_STATE(rs_cf, HZ, 4);

				if (__ratelimit(&rs_cf)) {
					pr_warn("PGCL143-CACHEFLOOR: cached file/shmem folio pfn=%#lx dropped to %d < floor %ld while cached (mapping=%px nr_refs=%u); over-drop:\n",
						folio_pfn(folio), new_refs, cfloor,
						folio->mapping, nr_refs);
					dump_stack();
				}
				folio_ref_add(folio, cfloor - new_refs);
				continue;
			}
			if (new_refs)
				continue;
			/*
			 * PGCL #143 deferred-put gate (PROVEN: Tessera
			 * property2/coq/rmap_defer.v no_free_while_referenced).  The
			 * refcount reached 0, but folio_mapped() -- the kernel's own
			 * witness for "still referenced" -- says a sub-PTE still maps
			 * this cluster (a forked mapper's reference was not counted in
			 * this aggregate put), so freeing now is the free-while-mapped
			 * race (#143).  Undo this put; the still-live mapper's own put
			 * frees it correctly later (leak-on-never beats corruption).
			 * Broader than the pending/quarantine tracker below --
			 * folio_mapped is the model witness for no_free_while_referenced.
			 */
			if (unlikely(folio_mapped(folio))) {
				VM_WARN_ONCE(1, "pgcl143: deferred free of still-mapped folio (pfn %lx mapcount %d nr_refs %u)",
					     folio_pfn(folio), folio_mapcount(folio),
					     nr_refs);
				folio_ref_add(folio, nr_refs);
				continue;
			}
			/*
			 * PGCL #143 ref-hold GATE (the real fix): the refcount reached
			 * 0, but if a deferred rmap removal is still PENDING on this
			 * cluster the free is premature -- the cross-mm early-drop /
			 * aggregate over-put racing ahead of tlb_flush_rmap_batch.
			 * Re-hold and skip; the pending removal's own tlb_finish_mmu
			 * free completes the cluster once pending hits 0.  The WARN's
			 * stack names the early-drop site (Tessera SharingRace §B).
			 */
			if (pgcl143_pending_test(folio_pfn(folio)) ||
			    pgcl143_quar_test(folio_pfn(folio))) {
				VM_WARN_ONCE(1, "pgcl143: premature cluster free, deferred rmap removal pending OR quarantined (pfn %lx mc %d nr_refs %u)",
					     folio_pfn(folio), folio_mapcount(folio),
					     nr_refs);
				folio_ref_inc(folio);
				continue;
			}
			/*
			 * PGCL #143 freed-while-on-LRU GATE (Tessera LruIsolation,
			 * isolate-before-free).  The refcount reached 0, but the folio
			 * is PG_active with PG_lru CLEAR: it is PENDING on a per-cpu
			 * lru_add batch (folio_add_lru queued it; it has not drained
			 * onto the real LRU yet).  The floor above can drive such a
			 * folio to 0 PAST the batch's own reference; freeing it now (a)
			 * reaches the buddy with PG_active set -- PAGE_FLAGS_CHECK_AT_FREE
			 * bad_page -- and (b) leaves the batch a dangling pointer, so
			 * when it drains it touches a freed/reused page and corrupts the
			 * pcp free-list (list_del in __rmqueue_pcplist / list_add in
			 * free_frozen_page_commit) -> allocator spins on pcp->lock ->
			 * soft-lockup/RCU-stall -> the GUI freeze.  Re-hold and skip; the
			 * lru_add drain sets PG_lru and frees it correctly (isolated)
			 * later.  Leak-on-never beats corruption.
			 */
			if (unlikely(folio_test_active(folio) &&
				     !folio_test_lru(folio))) {
				VM_WARN_ONCE(1, "pgcl143: deferred free of lru_add-pending folio (pfn %lx active !lru nr_refs %u)",
					     folio_pfn(folio), nr_refs);
				folio_ref_inc(folio);
				continue;
			}
		}
#else
		if (!folio_ref_sub_and_test(folio, nr_refs))
			continue;
#endif

		/* hugetlb has its own memcg */
		if (folio_test_hugetlb(folio)) {
			if (lruvec) {
				lruvec_unlock_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			free_huge_folio(folio);
			continue;
		}
		folio_unqueue_deferred_split(folio);
		__page_cache_release(folio, &lruvec, &flags);

		if (j != i)
			folios->folios[j] = folio;
		j++;
	}
	if (lruvec)
		lruvec_unlock_irqrestore(lruvec, flags);
	if (!j) {
		folio_batch_reinit(folios);
		return;
	}

	folios->nr = j;
	mem_cgroup_uncharge_folios(folios);
	free_unref_folios(folios);
}
EXPORT_SYMBOL(folios_put_refs);

/**
 * release_pages - batched put_page()
 * @arg: array of pages to release
 * @nr: number of pages
 *
 * Decrement the reference count on all the pages in @arg.  If it
 * fell to zero, remove the page from the LRU and free it.
 *
 * Note that the argument can be an array of pages, encoded pages,
 * or folio pointers. We ignore any encoded bits, and turn any of
 * them into just a folio that gets free'd.
 */
void release_pages(release_pages_arg arg, int nr)
{
	struct folio_batch fbatch;
	int refs[FOLIO_BATCH_SIZE];
	struct encoded_page **encoded = arg.encoded_pages;
	int i;

	folio_batch_init(&fbatch);
	for (i = 0; i < nr; i++) {
		/* Turn any of the argument types into a folio */
		struct folio *folio = page_folio(encoded_page_ptr(encoded[i]));

		/* Is our next entry actually "nr_pages" -> "nr_refs" ? */
		refs[fbatch.nr] = 1;
		if (unlikely(encoded_page_flags(encoded[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[fbatch.nr] = encoded_nr_pages(encoded[++i]);

		if (folio_batch_add(&fbatch, folio) > 0)
			continue;
		folios_put_refs(&fbatch, refs);
	}

	if (fbatch.nr)
		folios_put_refs(&fbatch, refs);
}
EXPORT_SYMBOL(release_pages);

/*
 * The folios which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those folios may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __folio_batch_release() will drain those queues here.
 * folio_batch_move_lru() calls folios_put() directly to avoid
 * mutual recursion.
 */
void __folio_batch_release(struct folio_batch *fbatch)
{
	if (!fbatch->percpu_pvec_drained) {
		lru_add_drain();
		fbatch->percpu_pvec_drained = true;
	}
	folios_put(fbatch);
}
EXPORT_SYMBOL(__folio_batch_release);

/**
 * folio_batch_remove_exceptionals() - Prune non-folios from a batch.
 * @fbatch: The batch to prune
 *
 * find_get_entries() fills a batch with both folios and shadow/swap/DAX
 * entries.  This function prunes all the non-folio entries from @fbatch
 * without leaving holes, so that it can be passed on to folio-only batch
 * operations.
 */
void folio_batch_remove_exceptionals(struct folio_batch *fbatch)
{
	unsigned int i, j;

	for (i = 0, j = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];
		if (!xa_is_value(folio))
			fbatch->folios[j++] = folio;
	}
	fbatch->nr = j;
}

#ifdef CONFIG_MEMCG
static void lruvec_reparent_lru(struct lruvec *child_lruvec,
				struct lruvec *parent_lruvec,
				enum lru_list lru, int nid)
{
	int zid;
	struct zone *zone;

	if (lru != LRU_UNEVICTABLE)
		list_splice_tail_init(&child_lruvec->lists[lru], &parent_lruvec->lists[lru]);

	for_each_managed_zone_pgdat(zone, NODE_DATA(nid), zid, MAX_NR_ZONES - 1) {
		unsigned long size = mem_cgroup_get_zone_lru_size(child_lruvec, lru, zid);

		mem_cgroup_update_lru_size(parent_lruvec, lru, zid, size);
	}
}

void lru_reparent_memcg(struct mem_cgroup *memcg, struct mem_cgroup *parent, int nid)
{
	enum lru_list lru;
	struct lruvec *child_lruvec, *parent_lruvec;

	child_lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
	parent_lruvec = mem_cgroup_lruvec(parent, NODE_DATA(nid));
	parent_lruvec->anon_cost += child_lruvec->anon_cost;
	parent_lruvec->file_cost += child_lruvec->file_cost;

	for_each_lru(lru)
		lruvec_reparent_lru(child_lruvec, parent_lruvec, lru, nid);
}
#endif

static const struct ctl_table swap_sysctl_table[] = {
	{
		.procname	= "page-cluster",
		.data		= &page_cluster,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void *)&page_cluster_max,
	}
};

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = PAGES_TO_MB(totalram_pages());

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */

	register_sysctl_init("vm", swap_sysctl_table);
}
