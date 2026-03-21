// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/arch/arm/kernel/irq.c
 *
 *  Copyright (C) 1992 Linus Torvalds
 *  Modifications for ARM processor Copyright (C) 1995-2000 Russell King.
 *
 *  Support for Dynamic Tick Timer Copyright (C) 2004-2005 Nokia Corporation.
 *  Dynamic Tick Timer written by Tony Lindgren <tony@atomide.com> and
 *  Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>.
 *
 *  This file contains the code used by various IRQ handling routines:
 *  asking for different IRQ's should be done through these routines
 *  instead of just grabbing them. Thus setups with different IRQ numbers
 *  shouldn't result in any weird surprises, and installing new handlers
 *  should be easier.
 *
 *  IRQ's are in fact implemented a bit like signal handlers for the kernel.
 *  Naturally it's not a 1:1 relation, but there are similarities.
 */
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/random.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/export.h>
#include <linux/vmalloc.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/hardware/cache-uniphier.h>
#include <asm/outercache.h>
#include <asm/softirq_stack.h>
#include <asm/exception.h>
#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>

#include "reboot.h"

unsigned long irq_err_count;

#ifdef CONFIG_IRQSTACKS

asmlinkage DEFINE_PER_CPU_READ_MOSTLY(u8 *, irq_stack_ptr);

static void __init init_irq_stacks(void)
{
	u8 *stack;
	int cpu;

	pr_alert("PGCL: init_irq_stacks: THREAD_SIZE=%lu THREAD_ALIGN=%lu VMAP=%d\n",
		 (unsigned long)THREAD_SIZE, (unsigned long)THREAD_ALIGN,
		 IS_ENABLED(CONFIG_VMAP_STACK));
	for_each_possible_cpu(cpu) {
		pr_alert("PGCL: irq_stack cpu=%d enter (order=%d sz=%d)\n", cpu, THREAD_SIZE_ORDER, THREAD_SIZE);
		stack = (u8 *)__get_free_pages(GFP_KERNEL,
					       THREAD_SIZE_ORDER);
		pr_alert("PGCL: irq_stack cpu=%d stack=%px\n", cpu, stack);
		if (WARN_ON(!stack))
			break;
		per_cpu(irq_stack_ptr, cpu) = &stack[THREAD_SIZE];
	}
}

#ifdef CONFIG_SOFTIRQ_ON_OWN_STACK
static void ____do_softirq(void *arg)
{
	__do_softirq();
}

void do_softirq_own_stack(void)
{
	call_with_stack(____do_softirq, NULL,
			__this_cpu_read(irq_stack_ptr));
}
#endif
#endif

int arch_show_interrupts(struct seq_file *p, int prec)
{
#ifdef CONFIG_FIQ
	show_fiq_list(p, prec);
#endif
#ifdef CONFIG_SMP
	show_ipi_list(p, prec);
#endif
	seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	return 0;
}

/*
 * handle_IRQ handles all hardware IRQ's.  Decoded IRQs should
 * not come via this function.  Instead, they should provide their
 * own 'handler'.  Used by platform code implementing C-based 1st
 * level decoding.
 */
void handle_IRQ(unsigned int irq, struct pt_regs *regs)
{
	struct irq_desc *desc;

	/*
	 * Some hardware gives randomly wrong interrupts.  Rather
	 * than crashing, do something sensible.
	 */
	if (unlikely(!irq || irq >= irq_get_nr_irqs()))
		desc = NULL;
	else
		desc = irq_to_desc(irq);

	if (likely(desc))
		handle_irq_desc(desc);
	else
		ack_bad_irq(irq);
}

void __init init_IRQ(void)
{
	int ret;

	pr_alert("PGCL: init_IRQ enter\n");
#ifdef CONFIG_IRQSTACKS
	pr_alert("PGCL: init_irq_stacks enter\n");
	init_irq_stacks();
	pr_alert("PGCL: init_irq_stacks done\n");
#endif

	pr_alert("PGCL: irqchip_init enter\n");
	if (IS_ENABLED(CONFIG_OF) && !machine_desc->init_irq)
		irqchip_init();
	else
		machine_desc->init_irq();
	pr_alert("PGCL: irqchip_init done\n");

	if (IS_ENABLED(CONFIG_OF) && IS_ENABLED(CONFIG_CACHE_L2X0) &&
	    (machine_desc->l2c_aux_mask || machine_desc->l2c_aux_val)) {
		if (!outer_cache.write_sec)
			outer_cache.write_sec = machine_desc->l2c_write_sec;
		ret = l2x0_of_init(machine_desc->l2c_aux_val,
				   machine_desc->l2c_aux_mask);
		if (ret && ret != -ENODEV)
			pr_err("L2C: failed to init: %d\n", ret);
	}

	uniphier_cache_init();
}

#ifdef CONFIG_SPARSE_IRQ
int __init arch_probe_nr_irqs(void)
{
	return irq_set_nr_irqs(machine_desc->nr_irqs ? : NR_IRQS);
}
#endif
