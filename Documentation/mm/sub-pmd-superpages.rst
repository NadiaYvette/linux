.. SPDX-License-Identifier: GPL-2.0

==============================
Sub-PMD Superpages and PGCL
==============================

This document describes the architectural framing, contracts, and locking
discipline for handling **sub-PMD superpages** in Linux: folios whose order
is greater than zero but smaller than ``HPAGE_PMD_ORDER``, mapped at PTE
granularity rather than via PMD-level THP entries.  These are commonly
known as **mTHP** (multi-size transparent huge pages).

When the **page clustering** infrastructure (PGCL) is enabled, sub-PMD
superpages interact with kernel-page granularity in subtle ways.  PGCL
allows the kernel allocation unit (``PAGE_SIZE``) to be larger than the
hardware MMU page size (``MMUPAGE_SIZE``); mTHP folios are larger still,
mapped by N consecutive PTEs of MMUPAGE granularity.  The combination
creates a layered structure (``mTHP folio`` → ``kernel pages`` → ``hardware
PTEs``) that requires careful contract design.

This document is the foundation for the PTE-walker correctness work
described in the `Phase 2 plan
<https://lore.kernel.org/...>`_ (RFC pending).

.. contents:: :local:

Concepts
========

mTHP and order
--------------

An mTHP folio is a compound folio of order ``N`` where
``0 < N < HPAGE_PMD_ORDER``.  On aarch64 with ``PAGE_SIZE=4KB``,
``HPAGE_PMD_ORDER`` is 9 (2MB); mTHP sizes range from order-1 (8KB) to
order-8 (1MB).  Each mTHP folio is mapped by ``2^N`` consecutive PTEs.

Sub-PMD superpages are *PTE-mapped*: there is no single PMD entry
covering the folio.  This contrasts with PMD-mapped THPs, which the
kernel handles via dedicated PMD-level entries.

PGCL clustering
---------------

PGCL configures the kernel allocation unit to be ``PAGE_MMUCOUNT``
(typically 1, 4, or 16) hardware pages.  Each ``struct page`` represents
one allocation unit; each PTE represents one hardware page.  The mTHP
layer sits above PGCL clustering: an mTHP folio of order N covers
``2^N`` kernel pages, which is ``2^N * PAGE_MMUCOUNT`` hardware PTEs.

The Layered Structure
---------------------

For an mTHP folio of order N at PGCL=K (``PAGE_MMUCOUNT=2^K``)::

  mTHP folio
    ├─ struct page #0 (kernel page 0)
    │    ├─ PTE 0 (sub-page 0)
    │    ├─ PTE 1 (sub-page 1)
    │    └─ ... (PAGE_MMUCOUNT total)
    ├─ struct page #1 (kernel page 1)
    │    └─ PAGE_MMUCOUNT PTEs
    └─ ... (2^N kernel pages total)

Total: ``2^N * 2^K = 2^(N+K)`` PTEs covering one mTHP folio.

PTE-Walker Contract
===================

Standard PTE-walker conventions
-------------------------------

All PTE walkers share these expectations:

1. The PMD's PTL is held during the walk.
2. ``mmap_lock`` is held (read or write, depending on intent).
3. PTE-walker functions return the number of PTEs consumed (``nr``);
   callers advance ``pte += nr`` and continue.

Batch awareness
---------------

A walker is **batch-aware** if it uses ``folio_pte_batch_flags()``
(``mm/internal.h``) to detect consecutive PTEs that map consecutive
sub-pages of the same large folio, and processes the entire batch as
one transaction.

Walkers that are *not* batch-aware iterate PTE-by-PTE.  This is
correct for operations that genuinely need per-PTE granularity
(e.g., checking individual access bits) but wrong for operations
that should act atomically on a logical mapping unit (e.g., rmap
removal, refcount accounting).

The full inventory of PTE walkers and their batch-awareness
classification is maintained in :doc:`sub-pmd-superpages-walkers`.

Refcount Accounting Contract
============================

PTE-ref convention
------------------

Each PTE that maps a folio holds an implicit reference: refcount
includes ``+1`` per mapped PTE.  When a PTE is added (via
``folio_add_*_rmap_pte()``), the corresponding ``folio_get()`` is
done by the caller.  When a PTE is removed (via
``folio_remove_rmap_pte()`` or ``folio_remove_rmap_ptes()``), the
corresponding ``folio_put()`` is queued via ``__tlb_remove_folio_pages()``
or done immediately, depending on context.

Caller-ref convention
---------------------

Operations that need to keep the folio alive across a possible
transition (e.g., wp_page_copy holding the folio while the PTL is
released) take a *caller-ref* via ``folio_get()`` at function entry
and release via ``folio_put()`` at function exit.  This is *separate*
from the PTE-ref.

Conflating these two refcount sources is a common bug class; see
*Known race classes* below.

split_folio ref-transfer contract
----------------------------------

``split_folio()`` (``mm/huge_memory.c``) consumes the caller's *single*
reference and **transfers it to the post-split sub-folio containing
the @page argument**.  Documented at
``__split_huge_page_to_list_to_order()``.  The standard caller pattern
is::

    folio_get(folio);                  /* +1 caller-ref */
    folio_lock(folio);
    err = split_folio(folio);          /* transfers caller-ref to head */
    folio_unlock(folio);
    folio_put(folio);                  /* releases the transferred ref */

After this sequence, head sub-folio's refcount = (head's PTE-mappings).

Lock Discipline
===============

Per-folio locks
---------------

* ``folio_lock`` (``PG_locked``): exclusive folio-level lock.  Protects
  against concurrent operations that need an atomic snapshot of the
  folio's mapping/state.  Required during ``__folio_split``,
  ``add_to_swap``, and other atomic-folio transitions.

* ``folio_lock_large_mapcount``: protects atomic updates of
  ``_large_mapcount``.  Narrow scope; not a general-purpose folio lock.

Per-mapping locks
-----------------

* ``anon_vma->rwsem``: protects the anon_vma chain that lists all VMAs
  mapping a given anon folio.  Taken by rmap-walk operations
  (``try_to_unmap``, ``try_to_migrate``, ``rmap_walk``).

* ``i_mmap_rwsem``: file-mapping equivalent of anon_vma->rwsem.

Per-page-table locks
--------------------

* ``PTL`` (page table lock, one per PMD via ``pte_lockptr()``): protects
  PTE entries within one PMD's range.  Required for any PTE
  modification.

* ``mmap_lock`` (``mmap_read_lock`` / ``mmap_write_lock``): protects the
  VMA list and high-level mm structure.  Read-locked during faults,
  write-locked during structural changes (mmap, munmap, mremap, fork's
  ``dup_mmap``).

Lock ordering
-------------

When multiple locks are held simultaneously::

  mmap_lock → anon_vma_lock → folio_lock → PTL

Departures from this ordering are bugs.  See the lockdep annotations
in ``mm/internal.h`` and ``include/linux/rmap.h``.

Known Race Classes (Phase 1 findings)
=====================================

The following race classes were identified during the Phase 1
investigation of ``cow.c`` selftest failures on aarch64 PGCL=0/2 with
``transparent_hugepage=always``.  Each is a target for Phase 2
remediation.

1. wp_page_copy ↔ try_to_migrate
--------------------------------

When ``wp_page_copy`` is in progress and ``try_to_migrate`` (called from
``unmap_folio`` during ``__folio_split``) replaces the same PTE with a
migration entry, the PTE-ref accounting double-decrements: ``try_to_migrate``
does ``folio_put`` per replaced PTE, and ``wp_page_copy``'s failure-path
also does ``folio_put`` for the (now stale) PTE-ref.  Net: ``-1`` ref.

This race is *not* fixable by adding the obvious "skip put if PTE
changed" check in ``wp_page_copy`` failure path, because that breaks the
caller-ref accounting at THP=never.  Phase 2's batch-aware
``try_to_migrate_one`` plus contract-precise wp_page_copy revisions
together must address this.

2. madvise_pageout ↔ deferred_split
-----------------------------------

``madvise_cold_or_pageout_pte_range`` calls ``split_folio`` on a
partially-mapped mTHP, releases PTL, then calls ``folio_put``.  During
the PTL-released window, concurrent operations can race.  Captured
data shows the post-split head folio with ``refc=1, _mapcount=0,
folio_mapped=true`` — the caller's ``folio_put`` then brings refc to 0
while ``folio_mapped`` reports the folio as mapped, triggering the
``rss-counter BUG`` at process teardown.

3. Head-folio ``_mapcount`` post-split semantics
-------------------------------------------------

``__split_unmapped_folio`` does not explicitly initialize the head
folio's ``_mapcount`` after compound clearing.  Tail folios are
validated via ``VM_BUG_ON_PAGE`` to have ``_mapcount=-1``; head bypasses
this validation.  Direct initialization to ``-1`` breaks file-folio
cases where head IS still mapped post-split.  The correct semantics
require deeper investigation (Phase 2f).

4. PTE-batch boundary observations
----------------------------------

Walkers using ``folio_pte_batch`` may observe a batch of N PTEs at one
moment but, by the time they process the N-th PTE, an mTHP split or
deferred split may have occurred.  Without folio_lock held across the
batch, observers see torn state.  Phase 2c's batch-aware
``try_to_unmap_one`` includes folio_lock annotations to close this.

Phase 2 Roadmap
===============

The remediation roadmap is tracked in
``Documentation/mm/sub-pmd-superpages-roadmap.rst`` (forthcoming).
High-level phases:

* **2a**: This document plus walker inventory.
* **2b**: Unified ``folio_pte_batch_flags`` API extension (BATCH_KIND
  return) with backward-compat shim.
* **2c**: ``try_to_unmap_one`` and ``try_to_migrate_one`` batch-aware
  conversions; ``page_vma_mapped_walk`` multi-PTE advance support.
* **2d**: madvise/mprotect/swap walker conversions.
* **2e**: Contract assertions and lockdep annotations.
* **2f**: Head-folio post-split semantics resolution.
* **2g**: Test infrastructure (mTHP-focused selftests).
* **2h**: Performance validation.
* **2i**: Upstream patch series preparation.

Test Coverage
=============

The following selftests exercise sub-PMD superpage scenarios:

* ``tools/testing/selftests/mm/cow.c``: comprehensive COW-on-mTHP
  tests including PARTIAL_MREMAP and PARTIAL_SHARED variants.
* ``tools/testing/selftests/mm/split_huge_page_test.c``: explicit
  split tests.

Phase 2g adds:

* ``mthp_mremap_wp.c``: focused mTHP+mremap+wp+fork race exerciser.
* ``mthp_partial_share.c``: madvise DONTFORK + fork patterns.

References
==========

* `THP discussion (lkml) <https://lore.kernel.org/...>`_
* `PGCL design rationale <Documentation/mm/page-clustering.rst>`_
  (forthcoming)
* The original "larpage" patch by Hugh Dickins (2001)
