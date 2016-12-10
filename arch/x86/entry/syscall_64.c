/* System call table for x86-64. */

#include <linux/linkage.h>
#include <linux/sys.h>
#include <linux/cache.h>
#include <linux/moduleparam.h>
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "syscall."
#include <linux/bug.h>
#include <linux/init.h>
#include <asm/asm-offsets.h>
#include <asm/syscall.h>
#include <asm/text-patching.h>

#define __SYSCALL_64_QUAL_(sym) sym
#define __SYSCALL_64_QUAL_ptregs(sym) ptregs_##sym

#define __SYSCALL_64(nr, sym, qual) extern asmlinkage long __SYSCALL_64_QUAL_##qual(sym)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
#include <asm/syscalls_64.h>
#undef __SYSCALL_64

#define __SYSCALL_64(nr, sym, qual) [nr] = __SYSCALL_64_QUAL_##qual(sym),

extern long sys_ni_syscall(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);

asmlinkage const sys_call_ptr_t sys_call_table[__NR_syscall_max+1] = {
	/*
	 * Smells like a compiler bug -- it doesn't work
	 * when the & below is removed.
	 */
	[0 ... __NR_syscall_max] = &sys_ni_syscall,
#include <asm/syscalls_64.h>
};

#ifdef CONFIG_X86_X32_ABI

/* Maybe enable x32 syscalls */

bool x32_enabled = !IS_ENABLED(CONFIG_X86_X32_DISABLED);
module_param_named(x32, x32_enabled, bool, 0444);

extern char system_call_fast_compare_end[], system_call_fast_compare[],
	system_call_mask_compare_end[], system_call_mask_compare[];

static int __init x32_enable(void)
{
	BUG_ON(system_call_fast_compare_end - system_call_fast_compare != 10);
	BUG_ON(system_call_mask_compare_end - system_call_mask_compare != 10);

	if (x32_enabled) {
		text_poke_early(system_call_fast_compare,
				system_call_mask_compare, 10);
#ifdef CONFIG_X86_X32_DISABLED
		pr_info("Enabled x32 syscalls\n");
#endif
	}
#ifndef CONFIG_X86_X32_DISABLED
	else
		pr_info("Disabled x32 syscalls\n");
#endif

	return 0;
}
late_initcall(x32_enable);

#endif
