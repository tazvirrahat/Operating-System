# Bare-Metal x86 Operating System — Technical Report

**Course:** Operating Systems
**Submitted:** *(date)*
**Authors:** *(names)*
**Repository:** *(URL)*

> **Companion document:** [`PROJECT_PLAN.md`](PROJECT_PLAN.md) — the development plan (component breakdown, schedule, technical reference). This document is the report: what was actually built, what went wrong, and what we did about it.
>
> **⚠ This document is a template with a pre-seeded challenge log. It describes work that has not happened yet.** Sections marked *(Fill in)* must be completed from real experience. See §3 for how to use the STAR entries.

---

## 1. Project overview

*(Fill in on completion — the draft below is a starting point.)*

We implemented a bare-metal operating system kernel for the 32-bit x86 architecture, developed under QEMU and booted via GRUB using the Multiboot standard. The kernel runs with no underlying operating system, no standard library and no runtime support: it establishes its own stack, installs its own descriptor tables, drives hardware peripherals through port I/O and memory-mapped registers, and manages its own memory.

The system demonstrates the core concerns of an operating system: **preemptive multitasking** driven by the programmable interval timer, **dynamic memory management** via a custom heap allocator, **synchronisation** through mutual exclusion primitives, **interrupt-driven device I/O** from a PS/2 keyboard, and **fault handling** through the interrupt descriptor table. *(If Tier 3 completed, add: and **privilege separation** between ring 0 and ring 3, mediated by software-interrupt system calls.)*

Interaction is through an interactive shell rendered in VGA text mode, including a live kernel monitor displaying scheduler and heap state in real time, and a `selftest` command that verifies each subsystem by assertion.

### 1.1 Architecture decision

*(Fill in — this is a genuine decision worth documenting.)*

The project initially targeted ARMv6 on an emulated Raspberry Pi and was changed to x86 before implementation began. The reasoning is recorded in [`PROJECT_PLAN.md`](PROJECT_PLAN.md) §1.1; in summary: first screen output reduces from ~40 lines of UART initialisation to a single memory write; keyboard input becomes feasible (PS/2 rather than a USB host stack); tutorial coverage is far denser; and the result can be booted on the team's own hardware. The commonly-cited x86 boot complexity was avoided entirely by delegating to GRUB via Multiboot.

We targeted 32-bit protected mode rather than 64-bit long mode because long mode requires paging to be configured before any output is possible, which forces the hardest component to be solved first.

### 1.2 Scope and deliberate exclusions

*(Fill in — state what you built and, importantly, what you consciously did not. Naming excluded components with reasons demonstrates engineering judgement; silence reads as ignorance.)*

Excluded by design: custom bootloader, 64-bit long mode, filesystem, GUI/windowing, web browser, and SMP. Rationale for each is recorded in [`PROJECT_PLAN.md`](PROJECT_PLAN.md) §3. The browser exclusion in particular is worth stating explicitly — it requires a USB stack, a network driver, a TCP/IP implementation and a TLS implementation before any HTML is parsed, and the dependency chain is what makes it infeasible rather than the rendering itself.

---

## 2. System architecture

*(Fill in on completion. Keep this brief — the plan holds the full reference. Describe what was actually implemented, not what was intended.)*

### 2.1 Boot sequence

*(Fill in: GRUB reads the Multiboot header → loads the kernel at `0x100000` in 32-bit protected mode → `_start` in `boot.S` sets the stack → `kmain` initialises subsystems in order → scheduler starts.)*

### 2.2 Components implemented

| Component | File(s) | Status | Notes |
|---|---|---|---|
| Multiboot header / entry | `boot.S` | | |
| Linker script | `linker.ld` | | |
| VGA text driver | `vga.c` | | |
| Serial debug channel | `serial.c` | | |
| GDT | `gdt.c` | | |
| IDT + ISR stubs | `idt.c`, `isr.S` | | |
| PIC remap | `pic.c` | | |
| PIT timer | `pit.c` | | |
| PS/2 keyboard | `keyboard.c` | | |
| Context switch | `context.S` | | |
| Scheduler | `task.c` | | |
| Heap allocator | `heap.c` | | |
| Synchronisation | `sync.c` | | |
| Shell | `shell.c` | | |
| Kernel monitor | `monitor.c` | | |
| `demo` / `selftest` | `demo.c`, `selftest.c` | | |
| System calls (ring 3) | `syscall.c` | | Tier 3 |
| Paging | `paging.c` | | Tier 3 |
| Real-hardware boot | — | | Tier 3 |

### 2.3 Key design decisions

*(Fill in. Candidates: GRUB/Multiboot over a custom bootloader; 32-bit over 64-bit; round-robin over priority scheduling; 4 MB PSE pages over two-level 4 KB paging; interrupt-driven keyboard over polling; serial logging alongside VGA output.)*

---

## 3. Challenges encountered (STAR format)

Each challenge is documented in STAR format:

- **Situation** — what the challenge was
- **Task** — what we were doing previously / the approach in place before the problem appeared
- **Action** — what we did to address it
- **Result** — the outcome of that action

### How to use this section

**Entries 3.2 onward are anticipated challenges**, pre-seeded with **Situation** and **Task** partially written. These are well-documented x86 bare-metal failure modes that we expect to encounter. Each carries a *"lines of investigation"* note to speed up diagnosis when it happens.

**Action and Result must be written by whoever actually hits the problem, describing what actually occurred.** Do not submit an entry with an invented Action or Result. **If a listed challenge never happens, delete the entry** — a shorter honest report is worth more than a padded one. Equally, add entries for problems not anticipated here; those are often the most interesting.

**Write entries the same day the problem is solved.** Reconstructed a week later they lose the specific detail — the exact error, the tool that revealed it, the false lead followed first — that makes them credible.

§3.1 is a **worked example demonstrating expected depth**. It has not happened. **Delete it before submission.**

---

### 3.1 EXAMPLE ENTRY — demonstrates expected depth — DELETE BEFORE SUBMISSION

**Situation.** GRUB loaded and displayed its menu, but selecting our kernel produced the error `error: no multiboot header found` and returned to the menu. The kernel compiled and linked without warnings, and the header struct was clearly present in the source, so the failure gave us no indication of what was actually wrong.

**Task.** We had been building with a simple `gcc` invocation and linking with default settings, assuming that any symbol defined in the source would appear in the output binary. Our first instinct was that the header constants were wrong, so we spent time re-checking the magic value and checksum arithmetic against the Multiboot specification — all of which were correct.

**Action.** We inspected the produced binary rather than the source. `objdump -s -j .multiboot kernel.elf` showed the section existed, but `readelf -S kernel.elf` revealed it had been placed after `.text`, roughly 40 KB into the file. The Multiboot specification requires the header within the **first 8 KB**. We added an explicit output section to `linker.ld` placing `.multiboot` first, before `.text`, and marked the header struct with `__attribute__((section(".multiboot"), aligned(4)))` so it could not be reordered or discarded.

**Result.** GRUB loaded the kernel on the next attempt. The wider lesson was that on bare metal the *layout* of the binary is part of the program's correctness, not an implementation detail the toolchain can be trusted to handle — the linker will happily produce a well-formed ELF file that no bootloader can load. We began checking `readelf -S` output as a routine step after any change to the build, which caught a second layout problem later in the project before it cost us time.

---

### 3.2 GRUB reports "no multiboot header found"

**Situation.** GRUB starts but refuses to load the kernel, reporting a missing Multiboot header despite the header being present in the source.

**Task.** *(Fill in: what was your build and linking setup before this?)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Header not within the first 8 KB of the binary (fix by ordering `.multiboot` before `.text` in the linker script); header not 4-byte aligned; checksum arithmetic wrong (must satisfy `magic + flags + checksum == 0` in 32-bit wraparound); the struct discarded by the linker as unused — mark it `__attribute__((used))` or reference it from `_start`.

---

### 3.3 QEMU reboots in a loop — triple fault

**Situation.** The QEMU window clears and restarts repeatedly, with no error message and no output surviving the reset. This is the characteristic x86 beginner failure and it destroys all evidence of what went wrong.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** A triple fault is an exception raised while handling an exception while handling an exception, and the CPU responds by resetting. Common causes: no IDT installed yet (any fault is unrecoverable); malformed IDT entries (wrong gate type, wrong selector, offset split incorrectly across the two halves); a bad GDT; stack pointer pointing at unmapped memory; paging enabled without the kernel identity-mapped.
>
> **Diagnostic tools:** `qemu -d int,cpu_reset` prints every interrupt and the CPU state at reset. `-no-reboot -no-shutdown` freezes the machine instead of rebooting so the state can be inspected. Serial logging to a file (`-serial file:log.txt`) preserves output across the reset. **Set up serial logging before you need it** — after a triple fault it is too late.

---

### 3.4 Nothing appears on screen despite writing to `0xB8000`

**Situation.** The VGA write executes and is definitely reached, but the screen stays blank or shows garbage.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Wrong cell format — each cell is *two* bytes (character in the low byte, attribute in the high byte), so writing bytes rather than 16-bit words produces garbage; attribute byte zero means black-on-black (invisible); missing `volatile` on the pointer, allowing the optimiser to delete the store; writing past the 80×25 bounds; after paging is enabled, `0xB8000` no longer mapped.

---

### 3.5 Timer IRQ collides with the divide-by-zero exception

**Situation.** Enabling interrupts causes apparently random divide-error exceptions, or the timer handler runs when no timer was expected.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** The PIC delivers IRQ 0 on vector 0 by default, which is also the CPU's divide-error exception vector — the two are genuinely indistinguishable. The PIC must be remapped so hardware IRQs arrive on vectors 32–47. This is mandatory on x86 and has no ARM equivalent, so tutorials from other architectures will not warn about it.

---

### 3.6 Timer interrupt fires exactly once

**Situation.** The first timer interrupt is delivered and handled correctly, but no further interrupts ever arrive.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** The End Of Interrupt (EOI) byte `0x20` was never sent to the PIC command port (`0x20` for master, and *both* `0xA0` and `0x20` for slave IRQs), so the PIC believes the interrupt is still in service and blocks further delivery. Also check that the handler returns with `iret` rather than `ret`, and that interrupts were re-enabled.

---

### 3.7 Interrupt handler corrupts the interrupted code

**Situation.** Interrupts are delivered and the handler appears to run correctly, but the interrupted code subsequently misbehaves or crashes.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** A C function used directly as an interrupt handler does not preserve the right state. The assembly stub must push the full register set (`pusha`), set up segment registers, call the C handler, restore (`popa`), and return with `iret` — which pops `EIP`, `CS` and `EFLAGS`, unlike `ret`. Exceptions that push an error code (8, 10–14, 17) need a different stub from those that do not, or the stack becomes misaligned.

---

### 3.8 Context switch corrupts task state

**Situation.** Tasks begin switching but then crash, produce corrupted output, or resume at the wrong address.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Incomplete register save/restore; `esp` swapped at the wrong point relative to the pushes; a newly created task's stack not pre-populated with a plausible initial frame (a fresh task has no saved state to restore, so the stack must be constructed by hand to look as if it had been interrupted); `EFLAGS` not preserved, losing the interrupt-enable flag; switching inside the IRQ handler without accounting for the frame `iret` expects to find.

---

### 3.9 Stack collision between tasks

**Situation.** Adding a third or fourth task causes unrelated tasks to misbehave, or the kernel crashes in previously stable code.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Task stacks allocated too small or placed adjacently with no guard region, so one task's descending stack grows into another's data. Consider writing a known guard word at the base of each stack and checking it on every context switch — this converts a silent corruption into an immediate, diagnosable failure.

---

### 3.10 Keyboard produces wrong or duplicated characters

**Situation.** Keystrokes register but produce incorrect characters, or each key appears twice.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Scancode set 1 sends a *make* code on press and a *break* code on release (make code + `0x80`); handling both as presses doubles every character. Extended keys (arrows, right-control) are prefixed with `0xE0` and need a state machine. Shift and caps-lock require tracking modifier state rather than a flat lookup table.

---

### 3.11 Race condition demo produces the correct result without a lock

**Situation.** The deliberately unsynchronised counter demonstration returns the expected total, so the race condition we intended to demonstrate does not manifest.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** The increment completes within a single timeslice, so preemption never lands inside the critical section. Widen the critical section (read → artificial delay → write) or raise the PIT frequency so a context switch reliably occurs mid-operation. This is a useful entry precisely because it required understanding *why* the race was absent rather than merely making it appear.

---

### 3.12 Ring 3 transition causes a general protection fault *(Tier 3)*

**Situation.** Attempting to enter user mode produces an immediate general protection fault, or the system triple-faults.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** Ring 3 requires user-mode code and data descriptors in the GDT with DPL 3; the segment selectors must have RPL 3 set (the low two bits); a TSS must be installed with `esp0`/`ss0` pointing at a valid kernel stack, or the CPU has nowhere to switch to when an interrupt arrives from ring 3; the `iret` frame used to enter user mode must push `SS`, `ESP`, `EFLAGS`, `CS`, `EIP` in that order.

---

### 3.13 Enabling paging triple-faults the machine *(Tier 3)*

**Situation.** The instruction that sets `CR0.PG` executes and the machine immediately resets, with no fault reported.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** The kernel is not identity-mapped, so the instruction immediately following the enable resolves to an unmapped linear address and faults, and the page-fault handler itself is unmapped, producing a triple fault. Also: `CR3` not set before enabling; `CR4.PSE` not set while using 4 MB page-directory entries; the VGA buffer at `0xB8000` left unmapped, removing all output; page-directory entries missing the present or write bits.

---

### 3.14 Real hardware behaves differently from QEMU *(Tier 3)*

**Situation.** The kernel runs correctly under QEMU but fails, hangs or displays nothing when booted from USB on the physical machine.

**Task.** *(Fill in)*

**Action.** *(Fill in)*

**Result.** *(Fill in)*

> **Lines of investigation:** QEMU is more permissive than real hardware about descriptor and timing details; the real machine may boot via UEFI rather than legacy BIOS (requiring CSM to be enabled, or a UEFI-capable GRUB image); PS/2 may be emulated over USB and behave differently; real memory maps differ from QEMU's. Note in the Result that there is no debugger available here — the only diagnostic is what the kernel manages to print before failing, which is itself a finding worth stating.

---

### 3.15 Toolchain and workflow learning curve

**Situation.** The team began this project without command-line experience: no terminal use, no compiling from a shell, no Git CLI, and no exposure to a debugger. Bare-metal development requires all four and provides none of the feedback an IDE gives.

**Task.** *(Fill in: how were you working before — GitHub Desktop, IDE run buttons, no build system?)*

**Action.** *(Fill in: WSL2 setup, learning `make`, `grub-mkrescue` for ISO generation, attaching GDB to QEMU's debug socket, moving to Git on the command line.)*

**Result.** *(Fill in)*

> This is a legitimate entry, not an admission of weakness. The gap was identified at the outset and day 1 of the schedule was reserved for it. Note in the Result which tool made the largest difference — for most bare-metal projects it is either the debugger or serial logging, because both convert silent failures into inspectable evidence.

---

### 3.16 *(Add unanticipated challenges here)*

**Situation.**
**Task.**
**Action.**
**Result.**

> Problems not on this list are often the most valuable entries — they show genuine diagnosis rather than working through a known checklist.

---

## 4. Results

*(Fill in on completion.)*

### 4.1 What works

*(Fill in — map against the "must have" checklist in [`PROJECT_PLAN.md`](PROJECT_PLAN.md) §6.)*

### 4.2 What does not work / known limitations

*(Fill in. Be direct. Limitations stated honestly read as engineering maturity; omissions the marker discovers read as carelessness.)*

### 4.3 Verification

Printed output alone proves nothing — a single loop printing `[A] 000 [B] 111` is indistinguishable from real preemption. Every feature is therefore verified by one of three methods:

1. **Ablation** — disable the mechanism and show the system fails exactly as theory predicts.
2. **Hardware-authored values** — display registers the CPU wrote, which we never assigned.
3. **Nondeterminism** — genuine concurrency produces varying results across runs; a fake is repeatable.

*(Fill in the Evidence column with actual observed output.)*

| Feature | Method | What establishes it | Evidence |
|---|---|---|---|
| Preemption | Ablation | `preempt off` → first task monopolises the CPU; `preempt on` → interleaving resumes | |
| Preemption | Side effect | A task that produces **no output**, only increments a counter, shows a non-zero count afterwards — it can only have advanced by receiving real CPU time | |
| Preemption | Hardware value | PIT tick count at each context switch shows evenly spaced switches | |
| Scheduler fairness | Measurement | Per-task tick counts are approximately equal | |
| Heap | Behavioural | Allocations do not overlap; freed blocks are reused; adjacent free blocks coalesce; exhaustion returns NULL | |
| Heap | Integrity | Write a pattern to block A, allocate and write block B, verify A is unchanged | |
| Mutex | Nondeterminism | 10 unlocked runs produce 10 different wrong totals; 10 locked runs produce the correct total every time | |
| Page fault | Hardware value | `CR2` contains the address typed at the shell — written by the CPU, not by us | |
| Fault recovery | Behavioural | Shell accepts a further command after a task is killed | |
| Ring 3 | Hardware value | `CS` low bits report ring 3; privileged instruction raises a GPF | |
| All | External observer | GDB attached to QEMU: breakpoint in the timer ISR, `info registers` shows the saved `EIP` inside a task's loop | |

*(Paste the full `selftest` output here.)*

---

## 5. Reflection

*(Fill in.)*

### 5.1 What we would do differently

*(Fill in. Candidates: setting up serial logging before the first triple fault rather than after; adopting GDB earlier; verifying binary layout with `readelf` as a routine step; committing more frequently; resolving team capacity on day 1.)*

### 5.2 What we learned

*(Fill in. Aim for concrete mechanism over generality — "an interrupt is the only mechanism by which the kernel regains control of the CPU, so without a timer a runaway task owns the machine permanently" is worth more than "we learned a lot about operating systems".)*

---

## 6. References

| Resource | Use |
|---|---|
| [The Little Book About OS Development](https://littleosbook.github.io) | Primary guide — GRUB multiboot, i386, VGA text, IDT, PIT, keyboard, paging, ring 3 |
| [OSDev Wiki](https://wiki.osdev.org) | Reference for PIC, PIT, PS/2, GDT, TSS, paging |
| Intel Software Developer's Manual, Vol. 3 | Authoritative on protected mode, descriptors, exceptions |
| Multiboot Specification 0.6.96 | Header format and boot handover |

*(Add any further sources actually used. Cite tutorials followed closely — following a tutorial is legitimate; not citing it is not.)*
