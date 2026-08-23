# How a program asks the kernel for service

User code must not program devices or smash kernel memory. The CPU keeps
it in **ring 3**; the kernel runs in **ring 0**. The only legal way across
that boundary on this kernel is a **system call**: the program traps, the
kernel does the privileged work, and control returns (or the task exits).

The live contrast is two shell commands. Same unprivileged task, two
routes. The three-minute cut in [`SHOWCASE.md`](SHOWCASE.md) does not include
this demo -- it is here for the report and for questions. Line map:
[`CODE_MAP.md`](CODE_MAP.md).

---

## The rings

x86 privilege is the low two bits of the code-segment selector, **CPL**.

| Where | CS (this kernel) | CPL |
|-------|------------------|-----|
| Kernel | `0x08` | 0 |
| User task | `0x1B` (`GDT_USER_CODE` `0x18` \| 3) | 3 |

Ring-3 code and data descriptors are installed in `gdt_init`:

```70:71:kernel/gdt.c
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);          /* ring 3 code (DPL=3) */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);          /* ring 3 data (DPL=3) */
```

In ring 3 the CPU refuses privileged instructions: port I/O (`in`/`out`),
`cli`/`sti`, control-register access. IOPL in EFLAGS is 0 on entry, so a
VGA port write is a **general protection fault**, not a successful poke
at the hardware.

That is why every ordinary program you use cannot “just write the disk”:
it asks the kernel, and the kernel’s filesystem and block driver do it.

---

## The door: `int 0x80`

The Interrupt Descriptor Table has 256 gates. Almost all are **DPL 0**
(`0x8E`): only ring 0 may invoke them. One gate is **DPL 3** (`0xEE`):
vector **0x80**, the system-call entry.

```15:21:kernel/idt.h
/* Gate type + attributes.
 *   0x8E = present, DPL 0, 32-bit interrupt gate (clears IF on entry)
 *   0xEE = same but DPL 3, so ring 3 code is allowed to invoke it.
 *          Only the syscall vector uses this; everything else must be
 *          unreachable from user mode. */
#define IDT_GATE_KERNEL 0x8E
#define IDT_GATE_USER   0xEE
```

Exceptions and hardware IRQs are installed as kernel gates:

```80:84:kernel/idt.c
        idt_set_gate((uint8_t)i, (uint32_t)exception_stubs[i], IDT_GATE_KERNEL);

    /* IRQs land on 32-47 because the PIC is remapped there. */
    for (int i = 0; i < 16; i++)
        idt_set_gate((uint8_t)(32 + i), (uint32_t)irq_stubs[i], IDT_GATE_KERNEL);
```

The syscall gate is the exception:

```57:68:kernel/syscall.c
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
```

The assembly stub for vector 128 is the same shape as the other ISRs
(push a dummy error code, push the vector, join `isr_common`):

```104:115:kernel/isr.asm
; System call gate, vector 0x80.
;
; The only vector whose IDT entry has DPL 3, making it the single door ring 3
; code is permitted to open into the kernel. Everything else is unreachable
; from user mode by construction.
; ---------------------------------------------------------------------------
global isr128
isr128:
    cli
    push dword 0                ; no error code
    push dword 128
    jmp isr_common
```

Linux on i386 used this same vector for many years (`int $0x80`). Newer
systems prefer `syscall`/`sysenter`; the idea is unchanged: one controlled
entry, not open season on the IDT.

---

## Register convention

Documented at the top of `kernel/syscall.h`:

- **eax** — syscall number going in; return value coming out
- **ebx, ecx, edx** — arguments
- Vector: `SYSCALL_VECTOR` `0x80`

```16:21:kernel/syscall.h
#define SYSCALL_VECTOR 0x80

#define SYS_WRITE   1   /* ebx = NUL-terminated string           -> bytes written */
#define SYS_EXIT    2   /* terminate the calling task            -> never returns */
#define SYS_GETPID  3   /*                                       -> task id */
#define SYS_GETCS   4   /*                                       -> current CS, ring in low 2 bits */
```

Dispatch is a switch on the saved `eax` from the interrupt frame
(`kernel/syscall.c` 13–55). `SYS_WRITE` walks a string in `ebx` and
calls `kputc`; `SYS_EXIT` calls `task_exit`; `SYS_GETPID` returns the
task id; `SYS_GETCS` returns the interrupted `cs` (privilege in the low
two bits).

```13:16:kernel/syscall.c
static void syscall_dispatch(registers_t *regs)
{
    switch (regs->eax) {
    case SYS_WRITE: {
```

```32:47:kernel/syscall.c
    case SYS_EXIT:
        /* task_exit never returns, so this syscall never resumes its caller. */
        task_exit();
        break;

    case SYS_GETPID: {
        task_t *t = task_current();
        regs->eax = t ? (uint32_t)t->id : 0;
        break;
    }

    case SYS_GETCS:
        /* The CS the interrupted code was running under, saved by the CPU when
         * it took the interrupt. Its low two bits are the privilege level. */
        regs->eax = regs->cs;
        break;
```

`SYS_WRITE` prints via `kputc` — privileged console output the user task
cannot do with a VGA port write. Pointers in `ebx` are **not** validated
(comment in the handler); that is a documented limitation, not a hidden
one.

The C wrappers the user program actually executes:

```498:510:kernel/demos.c
static inline uint32_t sys_call0(uint32_t num)
{
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline uint32_t sys_call1(uint32_t num, uint32_t arg)
{
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(arg) : "memory");
    return ret;
}
```

---

## Entering ring 3

`enter_user_mode` never returns. It loads user `ds`/`es`/`fs`/`gs`, sets
the TSS kernel stack so the next interrupt has a place to land, then
builds an **iret frame** (SS, ESP, EFLAGS, CS, EIP) and `iret`s into the
user entry point (`kernel/syscall.c` 70–112):

```97:102:kernel/syscall.c
        "pushl %2                   \n"     /* SS     = user data selector */
        "pushl %0                   \n"     /* ESP    = top of the user stack */
        "pushl $0x202               \n"     /* EFLAGS = reserved bit + IF set */
        "pushl %3                   \n"     /* CS     = user code selector */
        "pushl %1                   \n"     /* EIP    = entry point */
        "iret                       \n"
```

`user_task_entry` prints CS while still in ring 0, then calls
`enter_user_mode` with either the illegal program or the syscall program
(`kernel/demos.c` 556–576).

---

## Two runs

The shell command is `user`, with an optional flag:

```389:401:kernel/shell.c
static void cmd_user(int argc, char **argv)
{
    bool use_syscall = (argc > 1 && strcmp(argv[1], "--syscall") == 0);

    if (!use_syscall) {
        kprintf("\nrunning a task in ring 3 that touches hardware directly.\n");
        kprintf("the CPU should stop it. (try 'user --syscall' for the legal route)\n");
    } else {
        kprintf("\nsame task, same privilege level, asking the kernel instead.\n");
    }

    user_mode_demo(use_syscall);
}
```

### `user` — the CPU refuses

`user_program_direct_hardware` prints a line through `SYS_WRITE` (so you
can see it is already in ring 3), then executes `out` to VGA port `0x3D4`:

```518:530:kernel/demos.c
static void user_program_direct_hardware(void)
{
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] running unprivileged\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] writing directly to VGA port 0x3D4...\n");

    /* Port I/O is gated by IOPL, which is 0 in the EFLAGS we entered ring 3
     * with. The CPU raises a general protection fault here. We never get to
     * the next line. */
    __asm__ volatile ("movw $0x3D4, %dx; movb $0x0F, %al; outb %al, %dx");

    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] hardware write succeeded - NOT expected!\n");
    sys_call0(SYS_EXIT);
}
```

The instruction after `out` does not run. Exception 13, `cs=001b`, the
`usermode` task is killed, the shell continues.

### `user --syscall` — the kernel acts for it

```532:550:kernel/demos.c
static void user_program_via_syscall(void)
{
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] running unprivileged\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] asking the kernel instead of touching hardware...\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] hello from ring 3, printed by the kernel on my behalf\n");
    sys_call1(SYS_WRITE, (uint32_t)"  [ring3] exiting cleanly\n");

    /* Record that the whole path completed. The self-test used to check only
     * that the task count returned to normal, which is true whether the task
     * exited cleanly or was killed by a fault — so it reported a pass while
     * ring 3 was in fact page faulting on its own stack. */
    user_reached_exit = true;

    sys_call0(SYS_EXIT);

    /* Unreachable: SYS_EXIT never returns. */
    for (;;)
        ;
}
```

Still ring 3 (`CS` low bits 3). Printing happens because **the kernel**
handled `SYS_WRITE`. Exit is `SYS_EXIT` → `task_exit`.

![Both runs on a live kernel](images/ring3-syscall.png)

---

## Automated check of the same two paths

`test_protection` in the self-test spawns the syscall program first and
requires `user_mode_completed()` (the flag set just before `SYS_EXIT`),
then the direct-hardware program and requires that the flag is *not* set
— killed, not a clean exit:

```233:253:kernel/selftest.c
    /* Spawn a ring 3 task that reaches the kernel only through int 0x80 and
     * exits cleanly. If it completes, the whole path worked: the iret into
     * user mode, the DPL 3 syscall gate, the kernel stack switch via the TSS,
     * and the return to ring 3 afterwards. */
    int before = task_count();

    user_mode_demo(true);

    /* Both conditions matter. The task count returning to normal only says
     * the task is gone; it is equally true if the task was killed. The
     * completion flag is set by the ring 3 code itself immediately before it
     * asks to exit, so together they distinguish "ran the whole way" from
     * "died somewhere in the middle". */
    CHECK(task_count() == before && user_mode_completed(),
          "ring 3 task ran via syscalls and reached its exit call\n");

    /* And the same task without the syscall route is killed by the CPU. */
    user_mode_demo(false);

    CHECK(task_count() == before && !user_mode_completed(),
          "ring 3 task killed for direct hardware access, kernel survived\n");
```

![Self-test: both privilege paths](images/selftest-protection.png)

`user_mode_completed` is the flag the ring-3 code sets itself
(`kernel/demos.c` 578–581, 543). Checking only that the task count dropped
would also pass if the task had died on a fault.

---

## What this is for, in one picture

```
ring 3 program                    ring 0 kernel
---------------                   -------------
  mov $SYS_WRITE, %eax
  mov $msg, %ebx
  int $0x80          ---------->  isr128 → syscall_dispatch
                                  kputc each character
                     <----------  iret, eax = bytes written
```

That is `write(1, buf, n)` on Linux, or `WriteFile` on Windows: the
application never owns the device; it requests a service.
