/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SYNC_CORE_H
#define _ASM_X86_SYNC_CORE_H

#include <linux/preempt.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>

#ifdef CONFIG_X86_32
static inline void iret_to_self(void)
{
	asm volatile (
		"pushfl\n\t"
		"pushl %%cs\n\t"
		"pushl $1f\n\t"
		"iret\n\t"
		"1:"
		: ASM_CALL_CONSTRAINT : : "memory");
}
#else
static inline void iret_to_self(void)
{
	unsigned int tmp;

	asm volatile (
		"mov %%ss, %0\n\t"
		"pushq %q0\n\t"
		"pushq %%rsp\n\t"
		"addq $8, (%%rsp)\n\t"
		"pushfq\n\t"
		"mov %%cs, %0\n\t"
		"pushq %q0\n\t"
		"pushq $1f\n\t"
		"iretq\n\t"
		"1:"
		: "=&r" (tmp), ASM_CALL_CONSTRAINT : : "cc", "memory");
}
#endif /* CONFIG_X86_32 */

/*
 * This function forces the icache and prefetched instruction stream to
 * catch up with reality in two very specific cases:
 *
 *  a) Text was modified using one virtual address and is about to be executed
 *     from the same physical page at a different virtual address.
 *
 *  b) Text was modified on a different CPU, may subsequently be
 *     executed on this CPU, and you want to make sure the new version
 *     gets executed.  This generally means you're calling this in a IPI.
 *
 * If you're calling this for a different reason, you're probably doing
 * it wrong.
 */
static inline void sync_core(void)
{
	/*
	 * There are quite a few ways to do this.  IRET-to-self is nice
	 * because it works on every CPU, at any CPL (so it's compatible
	 * with paravirtualization), and it never exits to a hypervisor.
	 * The only down sides are that it's a bit slow (it seems to be
	 * a bit more than 2x slower than the fastest options) and that
	 * it unmasks NMIs.  The "push %cs" is needed because, in
	 * paravirtual environments, __KERNEL_CS may not be a valid CS
	 * value when we do IRET directly.
	 *
	 * In case NMI unmasking or performance ever becomes a problem,
	 * the next best option appears to be MOV-to-CR2 and an
	 * unconditional jump.  That sequence also works on all CPUs,
	 * but it will fault at CPL3 (i.e. Xen PV).
	 *
	 * CPUID is the conventional way, but it's nasty: it doesn't
	 * exist on some 486-like CPUs, and it usually exits to a
	 * hypervisor.
	 *
	 * Like all of Linux's memory ordering operations, this is a
	 * compiler barrier as well.
	 */
	iret_to_self();
}

/*
 * Ensure that a core serializing instruction is issued before returning
 * to user-mode. x86 implements return to user-space through sysexit,
 * sysrel, and sysretq, which are not core serializing.
 */
static inline void sync_core_before_usermode(void)
{
	/* With PTI, we unconditionally serialize before running user code. */
	if (static_cpu_has(X86_FEATURE_PTI))
		return;
	/*
	 * Return from interrupt and NMI is done through iret, which is core
	 * serializing.
	 */
	if (in_irq() || in_nmi())
		return;
	sync_core();
}

#endif /* _ASM_X86_SYNC_CORE_H */
