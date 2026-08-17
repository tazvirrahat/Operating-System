# Bare-Metal x86 Operating System — Technical Report

**Course:** Operating Systems
**Submitted:** *(date)*
**Authors:** *(names)*
**Repository:** *(URL)*

> **Companion document:** [`PROJECT_PLAN.md`](PROJECT_PLAN.md) — the development plan. This document is the report: what was built, what went wrong, and what was done about it.
>
> **Note on §3.** The challenges below are real: every one was encountered while building this kernel, and each is traceable to the commit that fixed it. Before submitting, read through them and make sure you can explain each fix in your own words — a marker is entitled to ask, and these are the parts of the project most worth understanding.
>
> **[`CHALLENGES.md`](CHALLENGES.md)** contains the same challenges as a standalone document using only the four STAR headings, which is the format the course brief asks for. If both are submitted, keep them in step — §3 here and the numbered sections there describe the same fourteen problems.

---

## 1. Project overview

We implemented a bare-metal operating system kernel for 32-bit x86, developed under QEMU and booted by GRUB using the Multiboot standard. The kernel runs with no underlying operating system, no standard library and no runtime support: it establishes its own stack, installs its own descriptor tables, drives hardware through port I/O and memory-mapped registers, and manages its own memory.

The system demonstrates the core concerns of an operating system:

- **Preemptive multitasking** — a round-robin scheduler driven by the programmable interval timer, switching tasks that never yield voluntarily
- **Dynamic memory management** — a first-fit heap allocator with block splitting, coalescing and corruption detection
- **Synchronisation** — spinlock, mutex and counting semaphore, with a race condition demonstrated failing before it is fixed, and a bounded-buffer producer/consumer where both sides block rather than poll
- **Interrupt-driven device I/O** — a PS/2 keyboard driver decoding raw scancodes into a ring buffer
- **Fault handling** — CPU exceptions caught, diagnosed and contained, killing the offending task while the kernel survives
- **Virtual memory** — paging with mixed page sizes, page 0 deliberately unmapped so that null dereferences fault
- **Privilege separation** — user tasks in ring 3 whose only route into the kernel is a single system call gate

Interaction is through a shell rendered in VGA text mode, with a live kernel monitor and a `selftest` command running 21 automated checks.

Roughly 4,700 lines across 44 files.

### 1.1 Architecture decision

The project initially targeted ARMv6 on an emulated Raspberry Pi and was changed to x86 before implementation began. In summary: first screen output reduces from roughly forty lines of UART initialisation to a single memory write; keyboard input becomes feasible at all (PS/2 rather than a USB host stack of some ten thousand lines); tutorial coverage is far denser; and the result can be booted on the team's own hardware.

The usual objection to x86 — the boot sector, real mode and mode-switching sequence — does not apply when using GRUB, which hands over a CPU already in 32-bit protected mode. The entire legacy boot problem is delegated to code we did not have to write.

We targeted 32-bit protected mode rather than 64-bit long mode because long mode requires paging to be configured *before* any output is possible, which forces the hardest component to be solved first, blind.

### 1.2 Scope and deliberate exclusions

Excluded by design: custom bootloader, 64-bit long mode, filesystem, GUI/windowing, web browser and SMP. Rationale for each is in [`PROJECT_PLAN.md`](PROJECT_PLAN.md) §3 and summarised in the README.

The browser exclusion is worth stating explicitly because it was asked about directly. A browser requires, in order: a USB host stack, a network driver, a TCP/IP implementation (lwIP, a deliberately minimal one, is around 40,000 lines), a TLS implementation (mbedTLS is around 100,000), an HTTP client, and only then HTML, CSS and JavaScript engines. The dependency chain is the obstacle, not the rendering. Recognising that is the useful part.

### 1.3 Known limitations

Stated here rather than left to be discovered:

- **Memory is not isolated between ring 0 and ring 3.** The whole first 4 MB is marked user-accessible, so a ring 3 task could read kernel memory. What *is* enforced is instruction privilege — user code cannot perform port I/O or execute privileged instructions, and the CPU kills it for trying. Proper isolation would require giving user code its own linker section on its own pages.
- **System call arguments are not validated.** `SYS_WRITE` dereferences a user-supplied pointer without checking it.
- **The scheduler is round-robin only** — no priorities, no aging. `sem_wait` yields in a loop rather than sleeping on a wait queue.
- **The race-variance self-test is statistical** and could in principle flake, since how many updates are lost depends on where preemption happens to land.

---

## 2. System architecture

### 2.1 Boot sequence

GRUB locates a multiboot header within the first 8 KB of the kernel image, loads the kernel at physical address `0x100000` with the CPU already in 32-bit protected mode, and jumps to `_start`. That stub sets the stack pointer, pushes the multiboot magic and info pointer, and calls `kmain`.

Subsystems then come up in dependency order, chosen so that each is diagnosable using the ones before it:

```
console (VGA + serial)   output first: nothing after this is debuggable without it
  -> gdt                 our own segments, replacing GRUB's temporary ones
  -> isr / idt           from here a fault prints a diagnostic instead of resetting
  -> pic                 remap IRQs off the exception vectors
  -> pit                 100 Hz heartbeat
  -> keyboard            interrupt-driven input
  -> paging              MMU on; needs the IDT so page faults have somewhere to land
  -> heap                needs mapped memory
  -> tasks + syscalls    need the heap for stacks
  -> selftest -> shell
```

### 2.2 Components implemented

| Component | File(s) | Notes |
|---|---|---|
| Multiboot header, entry | `boot.asm` | Stack setup, BSS handled by linker |
| Memory layout | `linker.ld` | Places `.multiboot` first; discards build-id and `.eh_frame` |
| VGA text driver | `vga.c` | `0xB8000`, scrolling, colour, hardware cursor |
| Serial driver | `serial.c` | COM1, polled; survives crash-reboot |
| Console | `console.c` | `kprintf` with width/alignment/fill, `panic` |
| GDT + TSS | `gdt.c` | 6 descriptors, ring 0 and ring 3, TSS for stack switching |
| IDT | `idt.c` | 256 entries; unused vectors marked not-present |
| Interrupt dispatch | `isr.asm`, `isr.c` | Uniform frames, separate error-code stubs |
| PIC | `pic.c` | Remapped to vectors 32–47 |
| Timer | `pit.c` | 100 Hz; exposes a callback rather than calling the scheduler |
| Keyboard | `keyboard.c` | Scancode set 1, make/break, shift, caps, ring buffer |
| Paging | `paging.c` | 4 KB pages for the first 4 MB, 4 MB pages above; page 0 unmapped |
| Heap | `heap.c` | First-fit, splitting, coalescing, magic headers |
| Context switch | `context.asm` | Callee-saved registers plus eflags, `esp` swap |
| Scheduler | `task.c` | Round-robin, preemptive, stack guard words, reaping |
| Synchronisation | `sync.c` | Spinlock, mutex, semaphore, atomic `xchg` |
| System calls | `syscall.c` | `int 0x80`, DPL 3 gate, ring 3 entry via `iret` |
| Shell | `shell.c` | 16 commands, tokeniser, dispatch table, 16-entry command history |
| Utilities | `string.c` | `memset`/`memcpy`/`strcmp`/`strtoul`; gcc emits calls to some of these regardless of `-fno-builtin` |
| Monitor | `monitor.c` | Live task/heap/IRQ display |
| Demos, self-test | `demos.c`, `selftest.c` | Shared by the shell and the automated checks |

### 2.3 Key design decisions

**GRUB and Multiboot over a custom bootloader.** Delegates the legacy boot sequence entirely. What is learned by writing a boot sector is BIOS trivia, not operating-system concepts.

**Dependency inversion at two boundaries.** The PIT exposes `pit_on_tick(callback)` rather than calling the scheduler directly, and the keyboard fills a ring buffer without knowing who drains it. Both keep the lower layer testable on its own and prevent the scheduler and shell from becoming load-bearing for device drivers.

**Two output channels behind one interface.** Every module calls `kprintf`; the console decides that output goes to both VGA and serial. Serial output is what survives a crash-reboot, and QEMU can log it to a file — which is what makes post-mortem debugging possible at all when the machine resets.

**Mixed page sizes.** 4 MB pages above the first 4 MB need no second level at all. The first 4 MB uses 4 KB granularity purely so that a *single* page at address zero can be left unmapped; a 4 MB page there would force a choice between mapping the kernel (which lives at 1 MB) and trapping null dereferences.

**Demos shared between the shell and the self-test.** A demonstration a human watches and an assertion a machine checks should not be two implementations that can drift apart.

---

## 3. Challenges encountered (STAR format)

Each challenge is documented as:

- **Situation** — what the challenge was
- **Task** — the approach in place before the problem appeared
- **Action** — what was done to address it
- **Result** — the outcome

A recurring theme runs through these and is worth stating up front: **on bare metal, "it compiled" carries almost no information about correctness.** There is no runtime, no loader and no operating system to catch anything, so the layout of the binary, the ordering of hardware operations, and what the optimiser decided to do with your code are all part of the program's correctness.

---

### 3.1 GRUB refused to load the kernel: header one byte out of range

**Situation.** The kernel compiled and linked cleanly, but GRUB rejected it with `no multiboot header found`. The header struct was plainly present in the source and the magic constant was correct, so the error gave no indication of what was actually wrong.

**Task.** We had written the multiboot header as an assembly section and assumed that any section defined in the source would appear near the start of the output binary. Debugging began by re-checking the magic value and the checksum arithmetic against the Multiboot specification — all of which were correct.

**Action.** We stopped reading the source and inspected the produced binary instead. `objdump -h` showed `.text` at file offset `0x2000`, and searching the raw bytes with `od` located the magic value at **file offset 8192** — while GRUB only scans the first 8192 bytes, offsets 0 to 8191. The header was one byte outside the search window.

The cause was a section we had not written: the linker was placing `.note.gnu.build-id` first, at `0x100000`, where it consumed the first page and pushed `.text` (and with it the multiboot header) onto the next one. We added `-Wl,--build-id=none` to the link flags and an explicit `/DISCARD/` block in the linker script covering the note sections and `.eh_frame`.

**Result.** GRUB loaded the kernel on the next attempt, with `.text` at file offset 4096, comfortably inside the window. `grub-file --is-x86-multiboot` was added to the build as a hard check so this class of failure can never again reach the point of being debugged interactively. More broadly, this established inspecting the binary rather than the source as the first debugging step, which paid off repeatedly.

---

### 3.2 Two source files silently overwriting each other's object file

**Situation.** After adding interrupt handling, the link failed with `undefined reference to isr_handler` — for a function that plainly existed and had compiled without error. Simultaneously it reported `multiple definition of irq15`, pointing at the same file as both the duplicate and the original.

**Task.** The Makefile derived object names by pattern substitution, mapping `kernel/%.c` and `kernel/%.asm` onto `build/%.o`.

**Action.** The contradictory pair of errors — a symbol both missing and duplicated — pointed at the build rather than the code. `kernel/isr.c` and `kernel/isr.asm` both mapped to `build/isr.o`. Whichever compiled last overwrote the other, so the C symbols and the assembly symbols could never both be present. We changed assembly objects to a `.asm.o` suffix.

**Result.** The link succeeded. The suffix also pre-empts the same collision for the `context.c`/`context.asm` pair added later, which would otherwise have reproduced the bug in a subtler form once the scheduler existed.

---

### 3.3 Acknowledging an interrupt after a handler that never returns

**Situation.** Anticipated rather than observed. While writing the scheduler it became clear that the timer would stop firing permanently on the first switch to a newly created task.

**Task.** The IRQ dispatcher followed the conventional shape: call the registered handler, then send the end-of-interrupt byte to the PIC.

**Action.** The timer handler drives the scheduler, and a context switch does not return — it swaps stacks and resumes a different task. If that task was newly created it has never been inside the dispatcher, so an EOI placed *after* the handler call would simply never execute. The PIC would continue to believe IRQ 0 was in service and deliver no further timer interrupts: the scheduler would run exactly once and the machine would freeze with no diagnostic. We moved the EOI to before the handler call, which is safe because interrupt gates clear the interrupt flag on entry, so no nested interrupt can arrive in the interim.

**Result.** Preemption worked on the first attempt. This is the one significant problem in the project that was reasoned about in advance rather than discovered by debugging, and the contrast is instructive: it would have presented as a total freeze with no output, which is among the hardest symptoms to work backwards from.

---

### 3.4 Format specifiers printed literally, corrupting every argument after them

**Situation.** The per-task CPU time table rendered as `%-10s id=1089572 state=%-8s ticks=1`, with the format specifiers appearing verbatim and the task IDs showing implausible values.

**Task.** `kprintf` had been written to handle a bare conversion character plus an optional zero-padded width, which was all the boot messages had needed.

**Action.** The specifiers were being echoed because `%-10s` was unsupported, but the more serious effect was invisible: the parser consumed the wrong number of variadic arguments, so every subsequent value in the call was read from the wrong stack slot. The implausible IDs were the *name pointers* being printed as integers. We rewrote the formatter to render each field into a buffer first and then apply width, alignment and fill separately, adding support for `-` and space fill, and taking care that a minus sign precedes zero padding rather than being buried inside it.

**Result.** Tables render correctly, which the kernel monitor depends on entirely. The instructive part is that the visible symptom (literal `%-10s`) was cosmetic while the invisible one (misaligned varargs) was producing confidently wrong numbers — output that looked like data.

---

### 3.5 The race condition demo produced the correct answer

**Situation.** The self-test asserted that two tasks incrementing a shared counter without a lock would lose updates. Every run returned exactly the expected total, so the demonstration proved nothing.

**Task.** Each racer performed a read, a short fixed-length delay loop, and a write, repeated several thousand times.

**Action.** The window between read and write was far too narrow relative to the timeslice: at 50 ms per slice and a critical section lasting a microsecond or two, the chance of preemption landing between the read and the write was roughly one in twenty-five thousand per iteration. A fixed spin count is the wrong instrument regardless, because how long it takes depends entirely on the host CPU.

We tied the width of the window to the same clock that drives preemption instead. The kernel now measures at runtime how many spin iterations fit inside one timer tick, then holds each update open for a randomised fraction of that. The timeslice was also shortened from five ticks to one.

**Result.** Updates are now reliably lost, and — after a further correction, below — by a varying amount.

---

### 3.6 The same demo then became *too* reliable

**Situation.** Having made the race fire, the total pinned to exactly half the expected value on every run. The self-test's separate assertion that results vary between runs began failing, on roughly one boot in three.

**Task.** Each iteration waited for the tick counter to change before writing back.

**Action.** Waiting on the clock edge made both racers wait on the *same* edge. They fell into lockstep, lost an update on every single iteration, and produced a result reproducible to the digit. That still demonstrates a race, but a number identical on every run is indistinguishable from a hardcoded one, which undermines the reason for showing it.

The window was narrowed to a randomised span of up to a quarter of a tick's work. The quarter matters: the calibration measures spinning while running alone, but two racers share the CPU, so the same work occupies roughly twice the wall-clock time — a window sized at a full tick again exceeds a timeslice and returns the loss probability to near certainty.

**Result.** Totals now scatter — `93 78 84 63 89` across five runs — with the locked variant exact every time. Verified stable across four consecutive boots. The lesson is that a demonstration can fail by being too deterministic as easily as by not firing at all.

---

### 3.7 A stale object file compiled against an old constant

**Situation.** After reducing a constant in `demos.h` from 8000 to 100, the self-test continued printing `expected 8000` while the demo itself used the new value. Source and binary disagreed with no warning from anywhere.

**Task.** The Makefile declared object files as depending on their `.c` file only.

**Action.** `selftest.c` had not itself changed, so make saw no reason to rebuild it, and `selftest.o` remained linked against the previous value of a macro defined in a header. We added `-MMD -MP` to the compile flags, which emit a dependency file per object listing every header it included, and an `-include` of those files in the Makefile.

**Result.** Editing a header now rebuilds everything that uses it. This is ordinary practice in any C project, but its absence is unusually confusing here because the failure presents as code that appears to ignore its own source.

---

### 3.8 The compiler deleted a deliberate divide-by-zero

**Situation.** The `fault div0` command reported `still alive - the fault did not fire`. No exception was raised, and the task ran to completion.

**Task.** The fault was written in C as a division by a variable holding zero, marked `volatile` specifically so the compiler could not fold the operation away.

**Action.** Disassembling the object file showed **no division instruction at all**. gcc had emitted a comparison and a conditional move: `1 / x` is nonzero only for `x` equal to 1 or -1, and division by zero is undefined behaviour, so the compiler is entitled to assume the case never occurs and rewrite the expression as a branchless select. `volatile` had forced the *load* of the operand, not the division that consumed it.

The instruction was written directly in inline assembly, leaving the compiler nothing to reason about.

**Result.** All three synchronous fault demos now raise genuine exceptions and are contained: the faulting task is killed and the shell survives. The general lesson is sharper than the specific fix — **you cannot reliably provoke a CPU exception from C**, because every way of doing so is undefined behaviour, and undefined behaviour is exactly what an optimiser is licensed to assume away. The same reasoning was applied pre-emptively to the null-pointer demo added later.

---

### 3.9 Ring 3 faulted on its first instruction: permissions are an AND across levels

**Situation.** The transition into user mode succeeded — the saved `CS` read `0x1B`, confirming ring 3 — but the very first instruction fetch raised a page fault with error code 5: present, read, user mode. A protection violation rather than a missing page.

**Task.** The paging code set the `PAGE_USER` flag on every page-table entry covering the first 4 MB, where both the user code and its stack reside.

**Action.** The error code was the clue: *present* meant the mapping existed, so the address was fine and the permission was not. The CPU computes the effective permission as the logical AND of every level of the page-table walk, and the page *directory* entry pointing at that table had only present and write bits. A user-accessible page reached through a kernel-only directory entry is still kernel-only. `PAGE_USER` was added to the directory entry.

**Result.** Ring 3 code executes. It is worth noting explicitly that this fix makes the entire first 4 MB user-accessible, which is why memory isolation is listed as a known limitation in §1.3 rather than claimed as working — instruction privilege is enforced, memory separation is not.

---

### 3.10 Every system call reported as an unknown exception

**Situation.** With ring 3 running, `int 0x80` reached the kernel but was reported as `CPU EXCEPTION 128: unknown`, and the calling task was killed as though it had faulted.

**Task.** The exception dispatcher looked up a registered handler before falling through to its diagnostic path.

**Action.** The lookup was written as `if (n < 32 && handlers[n])`, from when the only registered handlers were CPU exceptions. The syscall vector is 128, so the guard excluded it and the correctly-registered dispatcher was never consulted. Removing the bound fixed it.

**Result.** System calls dispatch correctly. The bug is trivial; what makes it worth recording is that it was introduced by an assumption that was true when written and quietly became false — the guard encoded "handlers are only ever exceptions", which no longer held once a syscall gate existed.

---

### 3.11 The monitor scrolled instead of redrawing

**Situation.** The live `top` display was intended to refresh in place. On screen it printed `[H` literally and scrolled, stacking successive frames down the display.

**Task.** The monitor emitted the ANSI escape `\033[H` through `kprintf` to home the cursor between frames.

**Action.** ANSI escapes are a terminal convention. The serial side is read by a terminal and honours them; the VGA side is a memory-mapped grid of character cells with no notion of escape sequences, so it faithfully rendered the bytes. We added a `console_home()` that moves the VGA hardware cursor directly and emits the escape only to the serial channel.

**Result.** The display refreshes in place on both channels. The underlying mistake was treating two genuinely different devices as one because they share an interface.

---

### 3.12 A demo that overstated its own evidence

**Situation.** The `race` command printed "totals differ from each other and from the expected value" whenever any run was incorrect — including runs where all three totals were identical, which a screenshot captured plainly.

**Task.** The summary branched on a single flag recording whether every run had returned the expected total.

**Action.** The message was making a claim the code had not checked. We added a second flag tracking whether the results actually differed from one another, and split the summary into three honest cases: no updates lost, updates lost with varying totals, and updates lost with identical totals.

**Result.** The command now reports what happened rather than what usually happens. This is a small fix, but the failure mode is worth naming: the output was *arguing for the correctness of the system using evidence that was not on screen*. That is precisely the kind of claim a reader should distrust, and the reason the verification approach in §4.3 avoids relying on narration.

---

### 3.13 A synchronisation demo revealed the console was itself unsynchronised

**Situation.** The newly added producer/consumer demonstration printed a marker per item. Its output came out as `c14 cP16 P17 P18 15` — one task's `c15` had been split down the middle, with another task's three markers wedged inside it.

**Task.** `kprintf` wrote straight through to the VGA buffer and the serial port with nothing serialising it. Every other shared structure in the kernel — the heap free list, the task list, the bounded buffer — was already protected, but the console had never been thought of as shared state.

**Action.** The splicing was correct behaviour for unsynchronised code: a task preempted partway through a call resumes later, and whatever ran in between wrote to the same screen. The obvious fix is a mutex around `kprintf`, and it is the wrong one. Fault handlers print, and a fault can occur *inside* a `kprintf` — the handler would then block on a lock held by the very task it interrupted, deadlocking against itself with no way out.

We added a nesting `preempt_disable()` / `preempt_enable()` counter to the scheduler instead, and wrapped `kprintf` in it. Interrupts stay enabled throughout, so the timer keeps ticking and CPU time is still charged to the task; only the reschedule is deferred. A counter nests where a lock deadlocks.

**Result.** Output is atomic per call: the demo now reads `P1 P2 P3 P4 c1 c2 c3 c4 P5 …`, which also makes the semaphore's four-slot bound visible directly rather than buried in noise.

Two things make this worth recording. First, the bug was found by a feature rather than by a test — writing the producer/consumer demo surfaced a defect in code that had been running since the first day. Second, it is a small lesson in what counts as shared state: the console had been treated as an output *service* rather than as a resource two tasks could contend for, and that framing is what hid it.

---

### 3.14 A polling loop that could hang the kernel outside the emulator

**Situation.** Preparing to run the kernel in VMware rather than only in QEMU, we reviewed the code for assumptions that hold in an emulator but not elsewhere. The serial driver waited in an unbounded loop for the UART to report its transmit register free.

**Task.** The driver assumed a serial port exists, because QEMU always provides one. Every message the kernel prints passes through that function.

**Action.** VMware gives a guest no serial port unless one is configured, and most modern PCs have none at all. Reading an absent port returns whatever the bus floats to — commonly `0xFF`, which coincidentally has the ready bit set, but `0x00` on some chipsets, which reads as permanently busy. In that case the loop never exits and the kernel hangs inside its first `kprintf`, before anything reaches the screen: a black display with no diagnostic, the hardest possible symptom to work backwards from.

We added a UART loopback probe at startup — put the chip in loopback, write a byte, check the same byte returns — and bounded the transmit wait. A missing port now disables serial output rather than blocking on it.

**Result.** The kernel boots identically with and without a UART, verified both ways under QEMU: 22 self-tests pass and the screen output is identical. Dropping debug output when the hardware is absent is survivable; hanging to deliver it is not.

The method is worth noting as much as the fix. Rather than testing the same configuration again, we asked what QEMU was being *forgiving* about — which is where the defects that only appear on real hardware live.

---

### 3.15 Toolchain and workflow

**Situation.** The project began with no command-line experience on the team: no terminal use, no compiling from a shell, no Git CLI, and no debugger. Bare-metal development requires all of these and provides none of the feedback an IDE gives.

**Task.** *(Fill in: how were you working before — GitHub Desktop, IDE run buttons, no build system?)*

**Action.** The build environment was moved into a Docker image containing the whole toolchain — cross-compiler, assembler, QEMU, GRUB, ISO tools and debugger — so that nothing had to be installed on the host and every machine gets an identical setup. *(Fill in what you personally did: running the build, reading errors, using git.)*

**Result.** *(Fill in.)* Worth noting: the single most valuable piece of infrastructure turned out to be serial logging to a file, because it is the only thing that survives a triple fault and reboot. It was set up on day one rather than after it was first needed.

---

### 3.16 *(Add anything else you hit)*

**Situation.**
**Task.**
**Action.**
**Result.**

---

## 4. Results

### 4.1 What works

All of the "must have" list in [`PROJECT_PLAN.md`](PROJECT_PLAN.md) §6, plus both Tier 3 stretch goals:

- Boots under GRUB in QEMU; multiboot magic verified against the value the bootloader leaves in `eax`
- VGA text output with colour and scrolling; serial debug channel loggable to a file
- Own GDT with ring 0 and ring 3 descriptors and a TSS
- 256-entry IDT; CPU exceptions produce a full diagnostic including register dump
- PIC remapped to vectors 32–47; PIT at 100 Hz; interrupt-driven PS/2 keyboard
- Paging enabled, 123 MB identity mapped, page 0 unmapped
- Heap allocator with splitting, coalescing, exhaustion handling and corruption detection
- Preemptive round-robin scheduling with stack guard words and reaping of finished tasks
- Spinlock, mutex and semaphore; race condition demonstrated failing then fixed
- Bounded-buffer producer/consumer: 24 items through 4 slots, both sides blocking
- Ring 3 tasks with `int 0x80` system calls
- Faults contained: the offending task dies, the kernel and shell continue
- Shell with 16 commands and arrow-key command history, live kernel monitor, `demo` walkthrough, `selftest`
- Atomic console output: `kprintf` defers preemption so concurrent tasks cannot splice each other's lines
- `make run`, `make test` and `make debug` from a clean clone

### 4.2 What does not work

The limitations in §1.3 are the honest list: memory is not isolated between rings, syscall pointers are unvalidated, the scheduler has no priorities, and the race-variance check is statistical. None of these were discovered late; all are consequences of scope decisions.

One further note: booting on real hardware (Tier 3 item 21) was **not attempted**. The kernel is built as a GRUB rescue ISO and should boot from a USB stick on a BIOS or CSM-enabled machine, but this has only ever run under QEMU. Claiming it works on real hardware without having tried it would be exactly the kind of unverified assertion this report tries to avoid.

### 4.3 Verification

Printed output proves nothing on its own — a single loop emitting `A B A B` is indistinguishable from real preemption. Every feature is therefore verified by one of three methods:

1. **Ablation** — disable the mechanism and show the system fails as theory predicts
2. **Hardware-authored values** — display registers the CPU wrote, which we never assigned
3. **Nondeterminism** — genuine concurrency gives different results across runs; a fake is repeatable

| Feature | Method | What establishes it | Observed |
|---|---|---|---|
| Preemption | Ablation | `preempt off` stops involuntary switches entirely; `preempt on` resumes them | PASS, both directions |
| Preemption | Side effect | A task producing **no output**, only incrementing a counter, shows a non-zero count afterwards | counter ≈ 1.2 × 10⁸ |
| Preemption | Interleaving | Three tasks in tight loops with no yields produce mixed output | `AAAABBBBCCCCAAAAABBBBB…` |
| Scheduler fairness | Measurement | Per-task tick counts comparable; no task starved | 10, 10, 11, 14 ticks |
| Heap | Behavioural | Non-overlap, cross-block integrity, address reuse after free, coalescing, NULL on exhaustion | 7 checks PASS |
| Heap | Integrity | Magic headers detect corruption; `heap_check()` walks the block list | PASS |
| Mutex | Nondeterminism | 5 unlocked runs give scattered wrong totals; locked runs exact | `93 78 84 63 89` vs `100 100` |
| Timer | Independence | Tick counter advances while the CPU sits in `hlt` | PASS |
| Page fault | Hardware value | `CR2` contains the address typed at the shell | typed `0xdeadb000`, CPU reported `deadb000` |
| Null dereference | Hardware value | `CR2` reads `0x00000000`, error code decodes as not-present, read | PASS |
| GPF | Hardware value | Error code is the offending selector | typed selector `0x80`, CPU reported `0x80` |
| Fault containment | Behavioural | Shell accepts further commands after a task is killed | PASS |
| Ring 3 | Hardware value | Saved `CS` reads `0x1B` — low two bits are the privilege level | PASS |
| Ring 3 | Ablation | Direct port I/O raises a GPF; the same task via syscall succeeds | PASS, both |

**Automated:** `selftest` runs 22 checks in-kernel. `make test` boots headless, captures serial output, and fails the build unless it reports zero failures. Verified stable across repeated boots.

**Screenshots** captured headlessly via QEMU's `screendump` are in [`docs/images/`](docs/images/).

---

## 5. Reflection

### 5.1 What we would do differently

**Set up binary inspection before writing code, not after the first mystery.** `objdump -h` and `readelf -S` resolved §3.1 in minutes after hours of reading correct source. On bare metal the layout of the output file is part of the program's correctness.

**Distrust the optimiser earlier.** §3.8 cost real time and the fix was one line. Anything that depends on undefined behaviour — deliberate faults especially — belongs in assembly from the start.

**Add header dependency tracking on the first day.** §3.7 produced a binary that contradicted its own source, which is a uniquely disorienting failure and is prevented by two compiler flags.

**Treat demonstrations as code that can be wrong.** Three separate defects (§3.5, §3.6, §3.12) were in the demonstration rather than the kernel: one proved nothing, one proved too much, one claimed evidence it had not gathered. Verification code deserves the same scepticism as the code it verifies.

### 5.2 What we learned

**An interrupt is the only mechanism by which a kernel regains control of the CPU.** This stops being abstract the moment you write `preempt off` and watch one task hold the machine forever. Without a timer, a runaway task owns it permanently — no amount of kernel code can intervene, because none of it is running.

**A task is a stack.** Context switching looks mysterious until you write it: save the registers the calling convention requires you to preserve, swap the stack pointer, pop the other set back, and `ret`. The `ret` returns somewhere entirely different from where you called from, and that is the whole trick. Creating a task means building a stack that *looks* as if it had already been switched away from.

**Privilege is enforced by hardware, not by the kernel.** Ring 3 code attempting port I/O is not stopped by a check we wrote — the CPU refuses and raises a fault. The kernel's role is to set up the descriptors, provide one controlled door, and decide what to do with the corpse.

**"It compiled" means very little without a runtime underneath.** Four of the defects above (§3.1, §3.2, §3.7, §3.8) were introduced by the toolchain doing exactly what it was asked, in a context where nothing existed to catch the consequences.

---

## 6. References

| Resource | Use |
|---|---|
| [The Little Book About OS Development](https://littleosbook.github.io) | GRUB multiboot, i386 setup, VGA text, IDT, PIT, keyboard, paging, ring 3 |
| [OSDev Wiki](https://wiki.osdev.org) | Reference for PIC remapping, PIT programming, PS/2 protocol, GDT/TSS layout, paging structures |
| Intel Software Developer's Manual, Vol. 3 | Authoritative on protected mode, descriptor formats, exception behaviour, control registers |
| Multiboot Specification 0.6.96 | Header format, the 8 KB search window, boot handover state |
