.. SPDX-License-Identifier: GPL-2.0

==========================================
PTE-Walker Inventory (Sub-PMD Superpages)
==========================================

This document is the companion inventory to
:doc:`sub-pmd-superpages`.  It catalogues every PTE-walking code path in
the kernel and classifies its **batch awareness**, **lock state**, and
**risk** for the sub-PMD superpage (mTHP) correctness work.

The classifications drive the Phase 2c/2d/2e remediation priority:
high-risk walkers that modify rmap/refcount without batch awareness are
converted first, low-risk read-only walkers last.

.. contents:: :local:

Inventory
=========

Lock-state notation: ``PTL`` is always implied for PTE walkers.  Other
locks are noted: ``mmap_R``/``mmap_W`` for read/write, ``folio_L`` for
folio_lock, ``anon_vma`` for anon_vma->rwsem.

Batch-awareness: ``yes`` uses ``folio_pte_batch`` or equivalent;
``partial`` recognizes batches in some sub-paths only; ``no`` walks
PTE-by-PTE.

mm/memory.c (fault, fork, zap)
------------------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``copy_present_ptes`` (1142)                 fork: copy parent PTEs       mmap_W + PTL     yes      yes (modify)    M
``copy_pte_range`` (1294)                    fork: PMD-level driver       mmap_W + PTL     yes      yes (modify)    M
``zap_present_ptes`` (1757)                  unmap: dispatch              mmap_R + PTL     yes      yes (modify)    H
``zap_present_folio_ptes`` (1710)            unmap: per-folio batch       mmap_R + PTL     yes      yes (modify)    H
``zap_pte_range`` (1999)                     unmap: PMD-level driver      mmap_R + PTL     yes      yes (modify)    H
``do_anonymous_page`` (5746)                 fault: fresh anon            mmap_R + PTL     yes      yes (modify)    M
``wp_page_copy`` (4022)                      fault: COW                   mmap_R + PTL     no       yes (modify)    H
``finish_fault`` (6219)                      fault: complete file/anon    mmap_R + PTL     yes      yes (modify)    M
``do_swap_page`` (varies)                    fault: swap-in               mmap_R + PTL     partial  yes (modify)    M
``set_pte_range`` (6138)                     fault helper: write PTEs     mmap_R + PTL     yes      yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/mremap.c
-----------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``move_ptes`` (203)                          mremap: relocate PTEs        mmap_W + PTL × 2 yes      no              M
``move_normal_pmd`` (373)                    mremap: PMD move             mmap_W + PMD     n/a      no              L
============================================ ============================ ================ ========= =============== ====

mm/rmap.c (rmap walks)
----------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``try_to_unmap_one`` (varies)                rmap: unmap callback         folio_L+anon+PTL no       yes (modify)    **H**
``try_to_migrate_one`` (varies)              rmap: migrate callback       folio_L+anon+PTL no       yes (modify)    **H**
``page_vma_mapped_walk`` (mm/page_vma_mapped.c) rmap: PTE-finding helper   PTL              no       no (read)       M
``__folio_remove_rmap`` (1804)               rmap: actual remove          PTL              n/a      yes (modify)    M
``__folio_add_rmap`` (1361)                  rmap: actual add             PTL              n/a      yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/migrate.c
------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``remove_migration_pte`` (346)               migrate: restore PTE         folio_L+anon+PTL no       yes (modify)    **H**
``remove_migration_ptes`` (455)              migrate: rmap-walk driver    folio_L+anon+PTL no       yes (modify)    H
``try_to_map_unused_to_zeropage`` (298)      migrate: zero-substitution   folio_L+anon+PTL no       yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/madvise.c (the cow.c bug-reproduction path)
----------------------------------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``madvise_cold_or_pageout_pte_range`` (353)  MADV_COLD / MADV_PAGEOUT     mmap_R + PTL     partial  yes (modify)    **H**
``madvise_dontneed_free_pte_range`` (varies) MADV_DONTNEED / MADV_FREE    mmap_R + PTL     partial  yes (modify)    **H**
``madvise_pageout_pmd`` (varies)             MADV_PAGEOUT for THP         mmap_R + PMD     n/a      yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/mprotect.c
-------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``change_pte_range`` (varies)                mprotect: change perms       mmap_W + PTL     no       no              M
============================================ ============================ ================ ========= =============== ====

mm/huge_memory.c (THP)
----------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``__split_huge_pmd_locked`` (3090)           split: PMD→PTEs              folio_L+PMD+PTL  yes      yes (modify)    M
``__folio_split`` (4078)                     split: order N→M             folio_L+anon     n/a      yes (modify)    **H**
``unmap_folio`` (3466)                       split helper: unmap all      folio_L+anon     n/a      yes (modify)    H
``remap_page`` (3564)                        split helper: restore        folio_L+anon     no       yes (modify)    H
``deferred_split_scan`` (varies)             shrinker: drain split queue  folio_L+anon     n/a      yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/khugepaged.c (collapse)
--------------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``collapse_huge_page`` (varies)              collapse: scan + replace     mmap_W + folio_L no       yes (modify)    M
``hpage_collapse_scan_pmd`` (varies)         collapse: scan candidates    mmap_R + PTL     no       no (read)       L
============================================ ============================ ================ ========= =============== ====

mm/userfaultfd.c
----------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``mfill_atomic_pte`` (varies)                uffd: install PTE            mmap_R + PTL     no       yes (modify)    M
``move_pages_pte`` (varies)                  uffd_move: move PTEs         mmap_W + PTL × 2 partial  yes (modify)    M
============================================ ============================ ================ ========= =============== ====

mm/swap_state.c, mm/swapfile.c
------------------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``unuse_pte_range`` (mm/swapfile.c)          swapoff: replace swap PTEs   mmap_W + PTL     no       yes (modify)    M
============================================ ============================ ================ ========= =============== ====

fs/proc/task_mmu.c (read-only observers)
----------------------------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``smaps_pte_range`` (varies)                 /proc/PID/smaps              mmap_R + PTL     no       no (read)       L
``pagemap_scan_pmd_entry`` (varies)          /proc/PID/pagemap            mmap_R + PTL     no       no (read)       L
``clear_refs_pte_range`` (varies)            /proc/PID/clear_refs         mmap_R + PTL     no       no (modify)     L
============================================ ============================ ================ ========= =============== ====

Other (NUMA, GUP, KSM)
----------------------

============================================ ============================ ================ ========= =============== ====
Function:line                                Scope                        Lock state       Batch    Touches rmap?   Risk
============================================ ============================ ================ ========= =============== ====
``queue_pages_pte_range`` (mm/mempolicy.c)   migrate: NUMA balancing      mmap_R + PTL     no       no              L
``follow_page_mask`` (mm/gup.c)              GUP: lookup                  mmap_R + PTL     no       yes (read)      L
``replace_page`` (mm/ksm.c)                  KSM: merge identical pages   folio_L+PTL      no       yes (modify)    M
============================================ ============================ ================ ========= =============== ====

Summary by Risk
===============

High-risk (Phase 2c targets — convert first)
--------------------------------------------

These walkers either modify rmap/refcount AND lack batch awareness, OR
are central to the bug-reproducing path:

* ``zap_present_ptes`` / ``zap_present_folio_ptes`` — high frequency,
  central to munmap/exit
* ``wp_page_copy`` — central to COW; failure-path race already
  documented in Phase 1
* ``try_to_unmap_one`` — invoked by ``unmap_folio`` during split; the
  split-side of the bug
* ``try_to_migrate_one`` — invoked by anon ``unmap_folio`` paths
* ``remove_migration_pte`` — invoked by ``remap_page``; lack of batch
  awareness causes orphan-PTE residuals
* ``madvise_cold_or_pageout_pte_range`` — the Phase 1 captured site
* ``madvise_dontneed_free_pte_range`` — analogous risk
* ``__folio_split`` — coordinator of the entire split; race-prone

Medium-risk (Phase 2d targets — convert after high-risk)
--------------------------------------------------------

* ``copy_present_ptes`` (fork)
* ``move_ptes`` (mremap)
* ``finish_fault`` and friends
* ``do_swap_page``
* ``change_pte_range`` (mprotect)
* ``__split_huge_pmd_locked``
* ``mfill_atomic_pte`` (uffd)
* ``unuse_pte_range`` (swapoff)
* ``replace_page`` (KSM)

Low-risk (Phase 2e+ — verify and optionally convert)
----------------------------------------------------

Read-only or non-rmap-modifying walkers:

* ``smaps_pte_range``, ``pagemap_scan_pmd_entry`` (procfs observers)
* ``hpage_collapse_scan_pmd`` (read-only scan)
* ``follow_page_mask`` (GUP read)
* ``queue_pages_pte_range`` (NUMA scanning)

Phase 2c Conversion Priority
============================

The recommended conversion order, balancing impact and dependency:

1. **``page_vma_mapped_walk``** — refactor to support multi-PTE
   advance.  This is a prerequisite for ``try_to_unmap_one`` and
   ``try_to_migrate_one`` batch awareness.
2. **``try_to_unmap_one``** — primary rmap-walk operation called by
   ``unmap_folio``.  Closes the central split-side race.
3. **``try_to_migrate_one``** — analogous to (2) for anon migration
   paths.
4. **``remove_migration_pte``** — paired with (2) and (3): if
   migration entries are batched on the unmap side, restoration must
   batch on the remap side.
5. **``madvise_cold_or_pageout_pte_range``** — the Phase 1 captured
   site.  Direct fix once (2) and (3) are in place.
6. **``madvise_dontneed_free_pte_range``** — analogous to (5).
7. **``__folio_split``** itself — re-audit after (2)–(6) are in place;
   contract assertions at entry and exit (Phase 2e).

Phase 2d converts the medium-risk list once 2c is stable.

Test Vectors
============

For each Phase 2c conversion, the following tests must pass:

* ``cow.c`` selftest on aarch64 PGCL=0/2/4 with
  ``transparent_hugepage=always`` (the original failure scenario)
* ``cow.c`` selftest on aarch64 PGCL=0/2/4 with
  ``transparent_hugepage=never`` (regression check)
* LTP ``mremap`` and ``madvise`` test families
* New focused selftests (Phase 2g): ``mthp_mremap_wp.c``,
  ``mthp_partial_share.c``
* Stress tests: parallel cow.c instances under SMP=4 to exercise
  inter-mm races

References
==========

* ``Documentation/mm/sub-pmd-superpages.rst`` — main framing document
* ``mm/internal.h`` — ``folio_pte_batch_flags`` API
* ``mm/page_vma_mapped.c`` — rmap walk infrastructure
