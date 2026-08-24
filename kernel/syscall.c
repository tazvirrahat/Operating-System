#include "syscall.h"
#include "isr.h"
#include "idt.h"
#include "gdt.h"
#include "task.h"
#include "console.h"
#include "fs.h"

/* Selectors with the requested privilege level (low two bits) set to 3.
 * The RPL must match or the CPU rejects the transition. */
#define USER_CODE_SELECTOR (GDT_USER_CODE | 3)   /* 0x1B */
#define USER_DATA_SELECTOR (GDT_USER_DATA | 3)   /* 0x23 */

static uint32_t calls_serviced;

uint32_t syscall_count(void) { return calls_serviced; }

static void syscall_dispatch(registers_t *regs)
{
    calls_serviced++;

    switch (regs->eax) {
    case SYS_WRITE: {
        const char *s = (const char *)regs->ebx;
        uint32_t written = 0;

        /* A real kernel would validate this pointer before dereferencing it.
         * We do not, which is a genuine limitation rather than an oversight —
         * see the note on memory isolation in syscall_init(). */
        while (s && *s) {
            kputc(*s++);
            written++;
        }

        regs->eax = written;
        break;
    }

    case SYS_EXIT:
        /* task_exit never returns, so this syscall never resumes its caller. */
        task_exit();
        break;

    case SYS_GETPID: {
        task_t *t = task_current();
        regs->eax = t ? (uint32_t)t->id : 0;
        break;
    }

    case SYS_WRITE_FILE: {
        /* The one service the desktop actually asks for through the gate.
         *
         * Notepad could call fs_write() directly -- it is kernel code and
         * nothing stops it. Routing Save through int 0x80 instead means the
         * save genuinely traps: the CPU takes the interrupt, lands in this
         * dispatcher, and the filesystem is reached from here. That is what
         * makes "saving goes through a system call" a statement about the
         * code rather than about the diagram. */
        const char *name = (const char *)regs->ebx;
        const void *data = (const void *)regs->ecx;
        uint32_t    len  = regs->edx;

        regs->eax = (name && fs_write(name, data, len)) ? 1u : 0u;
        break;
    }

    case SYS_GETCS:
        /* The CS the interrupted code was running under, saved by the CPU when
         * it took the interrupt. Its low two bits are the privilege level. */
        regs->eax = regs->cs;
        break;

    default:
        kprintf("\n[kernel] unknown syscall %u from task %d\n",
                regs->eax, task_current() ? task_current()->id : -1);
        regs->eax = (uint32_t)-1;
        break;
    }
}

void syscall_init(void)
{
    extern void isr128(void);

    /* DPL 3 is the whole point: this is the only gate ring 3 may invoke.
     * Every other vector is DPL 0, so a user task attempting `int 13` to fake
     * a fault gets a general protection fault instead. */
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)isr128, IDT_GATE_USER);
    isr_register(SYSCALL_VECTOR, syscall_dispatch);

    kprintf("syscalls         : int 0x%x, dpl 3, %d calls\n", SYSCALL_VECTOR, 4);
}

void enter_user_mode(void (*entry)(void), uint32_t user_stack_top)
{
    task_t *self = task_current();

    /* When an interrupt arrives while the CPU is in ring 3, it switches to the
     * stack named by the TSS before pushing anything. Without a valid esp0 the
     * first timer tick after entering user mode would double fault.
     *
     * We reuse the task's own kernel stack: it is idle while the task runs in
     * ring 3, because the frames that got us here are never returned to. */
    if (self && self->stack_base)
        tss_set_kernel_stack(self->stack_base + self->stack_size);

    /* iret does not care that we were never in ring 3 to begin with. Given a
     * stack frame that looks like an interrupt from user mode, it will happily
     * "return" there — which is how a kernel enters ring 3 in the first place.
     *
     * The frame must be exactly: SS, ESP, EFLAGS, CS, EIP (pushed in that
     * order, so EIP ends up on top). */
    __asm__ volatile (
        "cli                        \n"
        "mov %2, %%ax               \n"
        "mov %%ax, %%ds             \n"
        "mov %%ax, %%es             \n"
        "mov %%ax, %%fs             \n"
        "mov %%ax, %%gs             \n"

        "pushl %2                   \n"     /* SS     = user data selector */
        "pushl %0                   \n"     /* ESP    = top of the user stack */
        "pushl $0x202               \n"     /* EFLAGS = reserved bit + IF set */
        "pushl %3                   \n"     /* CS     = user code selector */
        "pushl %1                   \n"     /* EIP    = entry point */
        "iret                       \n"
        :
        : "r"(user_stack_top),
          "r"(entry),
          "i"(USER_DATA_SELECTOR),
          "i"(USER_CODE_SELECTOR)
        : "eax", "memory"
    );

    __builtin_unreachable();
}
