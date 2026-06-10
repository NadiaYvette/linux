/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASM_PARISC_SPARSEMEM_H
#define ASM_PARISC_SPARSEMEM_H

/* We have these possible memory map layouts:
 * Astro: 0-3.75, 67.75-68, 4-64
 * zx1: 0-1, 257-260, 4-256
 * Stretch (N-class): 0-2, 4-32, 34-xxx
 */

#define MAX_PHYSMEM_BITS	39	/* 512 GB */
/*
 * SECTION_SIZE_BITS must be >= MAX_PAGE_ORDER + PAGE_SHIFT.  Under PGCL,
 * PAGE_SHIFT can grow well beyond the bare MMU page; at PGCL=6 with
 * a 4 KiB MMU page it is 18, and MAX_PAGE_ORDER is 10, so we need at
 * least 28 bits (256 MiB sections).  Bumping unconditionally is harmless
 * for non-PGCL builds (still 256 MiB sections).
 */
#define SECTION_SIZE_BITS	28	/* 256 MB */

#endif
