/* syscall.h — the kernel's interface to unprivileged code.
 *
 * User-mode tasks run in ring 3, where the CPU refuses to execute privileged
 * instructions: no port I/O, no cli/sti, no control register access. A ring 3
 * task that touches hardware directly is killed by the CPU, not by us.
 *
 * The only way in is `int 0x80`, whose IDT gate is the one entry with DPL 3.
 * The syscall number goes in eax, arguments in ebx/ecx/edx, and the return
 * value comes back in eax.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

#define SYS_WRITE   1   /* ebx = NUL-terminated string           -> bytes written */
#define SYS_EXIT    2   /* terminate the calling task            -> never returns */
#define SYS_GETPID  3   /*                                       -> task id */
#define SYS_GETCS   4   /*                                       -> current CS, ring in low 2 bits */
#define SYS_WRITE_FILE 5 /* ebx = name, ecx = bytes, edx = length -> 1 ok, 0 failed */

/* How many system calls have been serviced since boot. Exposed so the
 * desktop can show that a save really did trap into the kernel rather
 * than calling the filesystem behind its back. */
uint32_t syscall_count(void);

void syscall_init(void);

/* Drop the calling task into ring 3 at `entry`, using `user_stack_top` as its
 * stack. Never returns: the transition is a one-way iret. */
void enter_user_mode(void (*entry)(void), uint32_t user_stack_top);

#endif /* SYSCALL_H */
