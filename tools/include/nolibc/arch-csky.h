/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * C-SKY (abiv2) specific definitions for NOLIBC
 */

#ifndef _NOLIBC_ARCH_CSKY_H
#define _NOLIBC_ARCH_CSKY_H

#include "compiler.h"
#include "crt.h"

/*
 * Syscalls for C-SKY (abiv2):
 *   - registers are 32bit wide
 *   - syscall number is passed in r7
 *   - arguments are in r0, r1, r2, r3, r4, r5
 *   - the system call is performed by executing "trap 0"
 *   - syscall return value is in r0 (a0)
 *
 * r0 is both the first argument and the return value, so it is bound with a
 * "+r" (read-write) constraint and the macro evaluates to it.
 */

#define __nolibc_syscall0(num)                                                \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _ret  __asm__ ("r0");                                   \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "=r"(_ret)                                                  \
		: "r"(_scno)                                                  \
		: "memory", "cc"                                              \
	);                                                                    \
	_ret;                                                                 \
})

#define __nolibc_syscall1(num, arg1)                                          \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno)                                                  \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall2(num, arg1, arg2)                                    \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno), "r"(_arg2)                                      \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall3(num, arg1, arg2, arg3)                              \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno), "r"(_arg2), "r"(_arg3)                          \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall4(num, arg1, arg2, arg3, arg4)                        \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno), "r"(_arg2), "r"(_arg3), "r"(_arg4)             \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall5(num, arg1, arg2, arg3, arg4, arg5)                  \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("r4") = (long)(arg5);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5) \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#define __nolibc_syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6)            \
({                                                                            \
	register long _scno __asm__ ("r7") = (num);                           \
	register long _arg1 __asm__ ("r0") = (long)(arg1);                    \
	register long _arg2 __asm__ ("r1") = (long)(arg2);                    \
	register long _arg3 __asm__ ("r2") = (long)(arg3);                    \
	register long _arg4 __asm__ ("r3") = (long)(arg4);                    \
	register long _arg5 __asm__ ("r4") = (long)(arg5);                    \
	register long _arg6 __asm__ ("r5") = (long)(arg6);                    \
										      \
	__asm__ volatile (                                                    \
		"trap 0"                                                      \
		: "+r"(_arg1)                                                 \
		: "r"(_scno), "r"(_arg2), "r"(_arg3), "r"(_arg4), "r"(_arg5),\
		  "r"(_arg6)                                                  \
		: "memory", "cc"                                              \
	);                                                                    \
	_arg1;                                                                \
})

#ifndef NOLIBC_NO_RUNTIME
/* startup code */
void _start_wrapper(void);
void __attribute__((weak,noreturn)) __nolibc_entrypoint __no_stack_protector _start_wrapper(void)
{
	__asm__ volatile (
		".global _start\n"
		".type _start, @function\n"
		".weak _start\n"
		"_start:\n"
		"mov a0, sp\n"               /* arg1 of _start_c = pointer to argc */
		"jbsr _start_c\n"            /* transfer to c runtime (does not return) */
		".size _start, .-_start\n"
	);
	__nolibc_entrypoint_epilogue();
}
#endif /* NOLIBC_NO_RUNTIME */

#endif /* _NOLIBC_ARCH_CSKY_H */
