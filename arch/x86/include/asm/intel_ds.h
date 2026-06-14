#ifndef _ASM_INTEL_DS_H
#define _ASM_INTEL_DS_H

#include <linux/percpu-defs.h>

/*
 * The BTS and PEBS trace buffers are hardware-written byte buffers sized in
 * hardware pages.  Under page clustering use MMUPAGE_SIZE, not the (large)
 * kernel PAGE_SIZE, so the buffers -- and the cpu_entry_area that maps them --
 * stay the intended ~64K rather than ballooning (e.g. to 4M at PAGE_MMUSHIFT=6).
 * Identity at PAGE_MMUSHIFT == 0 (MMUPAGE_SIZE == PAGE_SIZE).
 */
#define BTS_BUFFER_SIZE		(MMUPAGE_SIZE << 4)
#define PEBS_BUFFER_SHIFT	4
#define PEBS_BUFFER_SIZE	(MMUPAGE_SIZE << PEBS_BUFFER_SHIFT)

/*
 * The largest PEBS record could consume a hardware page, ensure
 * a record at least can be written after triggering PMI.
 */
#define ARCH_PEBS_THRESH_MULTI	((PEBS_BUFFER_SIZE - MMUPAGE_SIZE) >> PEBS_BUFFER_SHIFT)
#define ARCH_PEBS_THRESH_SINGLE	1

/* The maximal number of PEBS events: */
#define MAX_PEBS_EVENTS_FMT4	8
#define MAX_PEBS_EVENTS		32
#define MAX_PEBS_EVENTS_MASK	GENMASK_ULL(MAX_PEBS_EVENTS - 1, 0)
#define MAX_FIXED_PEBS_EVENTS	16

/*
 * A debug store configuration.
 *
 * We only support architectures that use 64bit fields.
 */
struct debug_store {
	u64	bts_buffer_base;
	u64	bts_index;
	u64	bts_absolute_maximum;
	u64	bts_interrupt_threshold;
	u64	pebs_buffer_base;
	u64	pebs_index;
	u64	pebs_absolute_maximum;
	u64	pebs_interrupt_threshold;
	u64	pebs_event_reset[MAX_PEBS_EVENTS + MAX_FIXED_PEBS_EVENTS];
	/*
	 * The DS area is a hardware structure mapped into the cpu_entry_area at
	 * hardware-page granularity; align to MMUPAGE_SIZE, not the kernel
	 * PAGE_SIZE.  Otherwise under page clustering each embedded/percpu copy
	 * balloons to a full (e.g. 256K) PAGE -- inflating the cpu_entry_area and
	 * pushing alloc_percpu(struct bts_ctx) past PCPU_MIN_UNIT_SIZE in
	 * bts_init() (the "illegal size for percpu allocation" WARN).  Identity at
	 * PAGE_MMUSHIFT == 0 (MMUPAGE_SIZE == PAGE_SIZE).
	 */
} __aligned(MMUPAGE_SIZE);

DECLARE_PER_CPU_PAGE_ALIGNED(struct debug_store, cpu_debug_store);

struct debug_store_buffers {
	char	bts_buffer[BTS_BUFFER_SIZE];
	char	pebs_buffer[PEBS_BUFFER_SIZE];
};

#endif
