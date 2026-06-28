# #143 R12 fix-shape — the teardown existence pin (incarnation-correctness)

From tessera (`from-tessera/143-tryget`). The machine-checked obligation behind the fix, the two
implementation routes, and an illustrative skeleton. Branched off `f17563985f5b` (the swap fix that
boots to GNOME). Validate on the **laptop `bad_page` count → 0** — the smp8 oracle is unfaithful (R12).

## The obligation (proved: tessera `proof/Tessera/Incarnation.lean`, axiom-clean)

`pinned_inc_correct` / `stableref_inc_correct`: **a teardown's existence reference must outlive ALL of
its own deferred operations on the cluster.** While that ref is held the cluster's `refcount > 0`, so
the pfn cannot be freed-and-reused (`CanReincarnate` needs `refs ≤ 0`) — and every in-flight teardown
op (the deferred rmap removal in `tlb_flush_rmaps`, AND the ref drop / free in
`free_pages_and_swap_cache`) therefore runs on the **same incarnation** it was scheduled against.
That is path-independent by construction — which is exactly what R12 demands (gating `delay_rmap`
only moved the over-remove, because it is not about one path).

The bug, in these terms: the `mmu_gather` *inherits* the per-sub-PTE mapping refs, but for a PGCL
**shared** cluster those do not genuinely pin the aggregate refcount (R11/R12: phantom / per-sub-PTE-
across-mms double-add). So another holder (`wp_page_copy`) drives the aggregate to 0 and reuses the
pfn while this gather still owes deferred work → `reincarnate_breaks` → `stale_remove_underflows`
(`mapcount -1` on the next incarnation).

## Two routes — same obligation, different discharge

### Route 2 — fix the aggregate-ref accounting (RECOMMENDED, cleaner)

Make each sub-PTE mapping contribute **exactly one genuine reference** to the cluster's aggregate
`refcount`, so the gather's `nr` inherited refs already pin it (`owed ≤ refs` holds for real). No extra
ref, no extra bit, no take/release matching. The over-remove then cannot happen: the aggregate cannot
reach 0 while any mm's gather still owes a deferred op. **This is your accounting, so you own the exact
site** — the candidate is the per-sub-PTE add/drop on a shared cluster (the "double-add" R11 named); the
invariant to restore is `Sharing`/`rmap_cluster.rs`'s `refcount == Σ live sub-PTEs across mms`, which
`Incarnation.pinned_inc_correct` then turns into incarnation-correctness for free. Prefer this if the
double-add is locatable — it removes the hazard rather than masking it.

### Route 1 — an explicit teardown pin (fallback, mechanical)

If the accounting is too tangled to fix surgically before the deadline, hold the pin explicitly:
take one real ref on the cluster when it is recorded for deferred teardown, release it only after the
gather's free. Skeleton (illustrative — the **take/release must be 1:1 per recorded cluster entry**,
which is why this needs an encoded-page marker, and why Route 2 is cleaner):

```c
/* mm/mmu_gather.c — __tlb_remove_folio_pages_size(), at record time */
struct folio *folio = page_folio(page);
bool pin = PAGE_MMUSHIFT && folio_test_large(folio);   /* a PGCL cluster */
if (pin) {
        folio_get(folio);                              /* genuine existence pin across the window */
        flags |= ENCODED_PAGE_BIT_TESSERA_PIN;         /* mark THIS entry so we release 1:1 */
}
...encode_page(page, flags)...

/* mm/mmu_gather.c — __tlb_batch_free_encoded_pages(), AFTER free_pages_and_swap_cache(pages, nr):
 * the non-pinned pages are now freed; the pinned cluster heads survive on our ref, so release
 * exactly the marked entries (no folio_test_large on a possibly-freed page). */
for (i = 0; i < nr; i++) {
        struct encoded_page *enc = pages[i];
        if (encoded_page_flags(enc) & ENCODED_PAGE_BIT_NR_PAGES_NEXT) { i++; /* skip nr_pages word */ }
        if (encoded_page_flags(enc) & ENCODED_PAGE_BIT_TESSERA_PIN)
                folio_put(page_folio(encoded_page_ptr(enc)));   /* release the teardown pin -> may free */
}
```

Correctness of Route 1 against the obligation: between the `folio_get` (record) and the `folio_put`
(after free), `refcount ≥ 1`, so `¬CanReincarnate` holds for the whole window → `pinned_inc_correct` →
every deferred op (rmap removal, then free) is incarnation-correct. The `_PIN` bit is only to release
1:1 (a take per recorded entry, a release per freed entry); without it the take/release counts can
mismatch on `nr==1` cluster yields (no `NR_PAGES_NEXT` flag), which would leak.

## Why NOT the other two candidates

- **(b) ordering** (clear-rmap-before-drop-ref as one unit): `delay_rmap=false` already *was* an
  ordering change and just relocated the over-remove to the free path — `Incarnation.ordered_inc_correct`
  is a *consequence* of holding the ref, not a standalone fix.
- **(c) incarnation tag** (check `inc == e` at each deferred op): robust (`taggedRemove_safe_on_reuse`)
  but invasive (a tag + check at every teardown decrement). Keep as a defensive `VM_WARN`, not the fix.

## Validate

A/B on the **laptop** (Electron/`caprine` madvise+COW pattern), not the oracle: `PGCL143-RMTRIP` and
`bad_page` → **0**, no LRU-lruvec-lock stall, desktop stays up. When that lands, hand back the count and
tessera confirms it discharges `Incarnation.pinned_inc_correct`. The swap fixes already boot to GNOME;
this closes the remaining original-#143 over-remove core.
