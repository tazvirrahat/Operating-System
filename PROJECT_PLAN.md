# Bare-Metal x86 Operating System — Project Plan

**Course:** Operating Systems
**Deadline:** 22 August 2026
**Plan revised:** 12 August 2026 — architecture changed from ARM to x86 (see §1.1)
**Target:** A bare-metal 32-bit x86 kernel that boots under GRUB, preempts tasks via hardware timer interrupts, manages its own heap, handles keyboard input, and runs an interactive shell with a live kernel monitor — developed in QEMU, with a stretch goal of booting on real hardware (AMD Ryzen 5 5600G).

> ## Status: Tier 1, Tier 2 and Tier 3 complete
>
> Every item in §6 is done except booting on real hardware. That includes both Tier 3 stretch features (ring 3 with system calls, and paging) and the remaining nice-to-haves (producer/consumer, command history). The kernel boots, runs 22 self-test checks, and drops into a shell with 16 commands. See [`README.md`](README.md) for what it does and [`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md) §3 for the real bugs hit on the way.
>
> **Not done:** booting on real hardware (§3, Tier 3 item 21). It has only ever run under QEMU.
>
> The schedule in §5 is left as originally written, because the gates in it are the part worth keeping — they are what a plan is for, and the report's reflection section refers back to them.

> **Companion document:** [`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md) — the submission write-up, including the challenges log in STAR format. This file is the plan (forward-looking, written before the work); that file is the report (backward-looking, describing what actually happened).

---

## 1. What we are building

A **bare-metal kernel** — a program that runs directly on hardware with no operating system underneath it. No Linux, no Windows, no standard library, no `printf`. When our code starts, the machine is empty: we own the CPU and we are responsible for everything, including putting a single character on screen.

Development happens in **QEMU** emulating a standard 32-bit x86 PC. Because that is the same architecture as the team's own hardware, the finished kernel can also be written to a USB stick and booted on a real machine.

### 1.1 Why x86 (revised from ARM)

The project initially targeted ARMv6 on an emulated Raspberry Pi. It was changed to x86 before any code was written, for four reasons:

| | ARM (rejected) | x86 (chosen) |
|---|---|---|
| **First text on screen** | UART initialisation, several registers, ~40 lines | `*(uint16_t*)0xB8000 = 0x0F41;` — **one line** |
| **Keyboard input** | Requires a USB host stack (~10,000 lines) — not feasible | Legacy PS/2 controller, ~30 lines |
| **Tutorial availability** | Thin | Extensive |
| **Runs on our own hardware** | No | **Yes — bootable USB on the 5600G** |

The commonly-cited objection to x86 — the 512-byte boot sector, real mode, and mode-switching sequence — **does not apply when using GRUB.** GRUB implements the Multiboot standard: it loads our kernel and hands over a CPU already in 32-bit protected mode, with a memory map supplied. We write a ten-line header and our kernel entry point. The entire legacy boot problem is delegated.

**Trade-off accepted:** privilege separation on x86 requires GDT and TSS setup rather than ARM's cleaner banked processor modes, and paging is nominally two-level rather than single-level. The second is mitigated by using **4 MB pages (PSE)**, which collapses paging to a single level.

### 1.2 Why 32-bit rather than 64-bit

We target **i386 protected mode**, not x86-64 long mode. Long mode *requires* paging to be enabled before it can be entered, forcing us to solve page tables before we can print a character. 32-bit protected mode has no such requirement, so we can defer paging to Tier 3 where it belongs. The concepts demonstrated are identical.

---

## 2. What the finished demo looks like

This is the actual deliverable. Everything in this section is the target output.

### 2.1 Boot and shell

```
  MyOS v1.0 — bare metal x86 (i386, protected mode)
  booted via GRUB multiboot   heap: 1024K   irq: enabled

  > help
  demo  selftest  spawn  preempt  meminfo  race  fault  top  clear
  >
```

### 2.2 Preemptive multitasking — the core demo

Three tasks spinning in tight loops that **never voluntarily yield**:

```
> spawn 3

[A] 000000000000111111111111
[B]             222222222222333333333
[C] 4444444444444444
[A] 22222222222
[B]      5555555555555

> _        (shell still accepts keystrokes while they run)
```

The interleaving is the proof. No task asked to stop — the PIT timer interrupt seized the CPU mid-loop and handed it to another task.

### 2.3 Ablation — preemption switched off

The strongest single demonstration in the project. Mask the timer IRQ and the system fails exactly as theory predicts:

```
> preempt off
> spawn 3
[A] 00000000000000000000000000000000000000...
      ^ task A runs forever; B and C never get the CPU

> preempt on
[A] 0000[B] 1111[C] 2222[A] 0000...
      ^ switching resumes mid-loop
```

### 2.4 Heap allocator

```
> meminfo
heap: 0x00200000 - 0x00300000  (1024 KB)
used:  312 KB (7 blocks)
free:  712 KB (3 blocks, largest 640 KB)

[0x200000] 128K USED  task A stack
[0x220000]  64K USED  task B stack
[0x230000]  16K FREE
[0x234000] 640K FREE
```

### 2.5 Race condition and mutex — visibly fails, then works

```
> race off --repeat 10
run 1: 13847   run 6: 15201
run 2: 16092   run 7: 11938
run 3: 12994   run 8: 17740
run 4: 19003   run 9: 14455
run 5: 15776   run10: 16887
        ^ all different, all wrong — genuine nondeterminism

> race on --repeat 10
all 10 runs: 20000
```

The **variance** is the evidence. A hardcoded fake would print the same wrong number every time.

### 2.6 Fault handling

```
> fault 0xDEADBEEF
*** PAGE FAULT
  CR2: 0xDEADBEEF          <- CPU wrote this register, not us
  error: 0x2 (write, not-present)
  EIP: 0x0010A31C
  task killed — kernel survived

> _        (shell still alive; type another command to prove it)
```

You typed the address; the CPU independently reported it back from `CR2`, a register you never assigned.

### 2.7 Privilege separation *(Tier 3)*

```
> user
[usr] CS = 0x1B (ring 3)              <- set by the CPU on iret
[usr] attempting `out` to port 0x60...
*** GENERAL PROTECTION FAULT
  task killed — kernel survived

> user --syscall
[usr] CS = 0x1B (ring 3)
[usr] int 0x80, eax=1 (write) ...
[knl] CS = 0x08 (ring 0) — kernel handling
[usr] hello from ring 3 via syscall
[usr] exited cleanly
```

Same operation, two outcomes, decided by the hardware.

### 2.8 Live kernel monitor (`top`)

VGA text mode with colour attributes — no graphics driver — updating on every timer tick, reading directly from the real scheduler task table:

```
┌─ MyOS ──────────────────────── uptime 00:01:23 ─┐
│ PID  NAME      STATE     TICKS   STACK          │
│  1   shell     RUNNING    1240   2K/4K  ▓▓▓▓░░  │
│  2   worker_a  READY       891   1K/4K  ▓▓░░░░  │
│  3   worker_b  BLOCKED     445   1K/4K  ▓░░░░░  │
├─────────────────────────────────────────────────┤
│ heap  ▓▓▓▓▓▓▓▓░░░░░░░░░░░░  312K / 1024K        │
│ irqs  timer 1240   kbd 89   syscall 412         │
└─────────────────────────────────────────────────┘
> _
```

### 2.9 `selftest` — the system verifying itself

```
> selftest

SCHEDULER
  [PASS] 3 tasks each received CPU time
  [PASS] silent task counter advanced (8417203 > 0)
  [PASS] switches occur at timer boundaries (±2 ticks)
  [PASS] preempt off -> only first task runs
HEAP
  [PASS] allocations do not overlap
  [PASS] freed block is reused
  [PASS] adjacent free blocks coalesce
  [PASS] exhaustion returns NULL
SYNC
  [PASS] unlocked counter wrong and varying (10 runs)
  [PASS] locked counter correct 10/10
FAULTS
  [PASS] CR2 matches faulting address
  [PASS] kernel survives task fault

13 passed, 0 failed
```

### 2.10 On the absence of a GUI

**Deliberate.** A GUI is an *application* concern that sits on top of an OS; scheduling, memory, privilege and fault handling *are* the OS. Linux boots to a text console; xv6, Pintos and OS/161 are all text-only. VGA text mode gives us 16 colours, box-drawing characters and direct memory-mapped output at zero driver cost, while displaying real kernel state.

A framebuffer (VESA mode via GRUB) is technically available and would take ~10 hours. It is excluded because that time is better spent on Tier 3, and because drawing rectangles is a weaker portfolio signal than privilege separation.

---

## 3. Component breakdown

### Tier 1 — Infrastructure (mandatory, hardest to debug)

| # | Component | Description | Est. |
|---|---|---|---|
| 1 | **Multiboot header + `_start`** (`boot.S`) | Magic header GRUB looks for; sets up a stack, calls `kmain`. | 2h |
| 2 | **Linker script** (`linker.ld`) | Places the kernel at `0x100000` (1 MB); defines BSS and heap symbols. | 1h |
| 3 | **VGA text driver** (`vga.c`) | Write characters + colour attributes to `0xB8000`. Scrolling, cursor. | 2h |
| 4 | **Serial driver** (`serial.c`) | COM1 at `0x3F8`. Debug channel — QEMU can log it to a file. | 1h |
| 5 | **GDT** (`gdt.c`) | Our own segment descriptors, replacing GRUB's temporary ones. | 2h |
| 6 | **IDT + ISR stubs** (`idt.c`, `isr.S`) | 256-entry interrupt table; assembly stubs that save state and dispatch to C. | 4h |
| 7 | **PIC remap** (`pic.c`) | Move hardware IRQs from vectors 0–15 to 32–47. **Mandatory** — otherwise IRQ 0 collides with the divide-by-zero exception. | 2h |
| 8 | **PIT timer** (`pit.c`) | Program channel 0 for ~100 Hz; handler increments tick count. | 2h |

**Tier 1 is the wall.** Everything up to the first character on screen is toolchain and boot configuration, where failures are silent or manifest as a rebooting VM. Once text appears, the project moves quickly.

### Tier 2 — Core OS features (the actual project)

| # | Component | Description | Est. |
|---|---|---|---|
| 9 | **PS/2 keyboard** (`keyboard.c`) | Port `0x60`/`0x64`, scancode set 1, make/break decoding, ring buffer. | 3h |
| 10 | **Task structs + context switch** (`context.S`) | Save/restore registers, swap `esp`. | 4h |
| 11 | **Round-robin scheduler** (`task.c`) | Task list, states (RUNNING/READY/BLOCKED), pick-next. | 2h |
| 12 | **Preemption** | Drive the scheduler from the PIT handler (#8). | 2h |
| 13 | **Heap allocator** (`heap.c`) | `kmalloc`/`kfree`, free list, splitting and coalescing. | 3h |
| 14 | **Sync primitives** (`sync.c`) | Spinlock, mutex, semaphore. Required once #10 exists. | 2h |
| 15 | **Shell** (`shell.c`) | Line buffer, tokeniser, command dispatch table. | 3h |
| 16 | **ANSI/VGA monitor** (`monitor.c`) | Live `top` display of task, heap and IRQ state. | 2h |
| 17 | **`demo` command** | Scripted, self-narrating walkthrough of every feature. | 1h |
| 18 | **`selftest` command** | Automated assertions with PASS/FAIL output. | 2h |

**Note on #17 and #18.** These are not padding. `demo` reduces a live presentation to typing one word — no commands to remember under pressure. `selftest` is the regression net (day-8 changes routinely break day-5 code in kernel work), and its output *is* the Verification section of the technical report.

### Tier 3 — Advanced (attempt in this order, only when Tier 2 is done)

| # | Component | Description | Est. | Risk |
|---|---|---|---|---|
| 19 | **TSS + ring 3 + syscalls** | Task State Segment, ring 0↔3 transitions, `int 0x80` dispatch. | 6h | med |
| 20 | **Paging (4 MB PSE pages)** | Page directory, `CR3`, enable via `CR0.PG`, page-fault handler reading `CR2`. | 6h | **high** |
| 21 | **Boot on real hardware** | `grub-mkrescue` ISO → USB → boot the 5600G. | 3h | med |

**Note on #20:** Use **4 MB pages** (set `CR4.PSE`, then the `PS` bit in page-directory entries). This collapses paging to a single level — one 1024-entry directory, no page tables — which is dramatically simpler than the standard two-level 4 KB scheme and is sufficient to demonstrate virtual memory and page faults.

**Note on #21:** A video of the team's own PC booting the kernel is the strongest portfolio artefact available, but it is a **demonstration target, not a development target** — there is no debugger on real hardware and each iteration costs a reboot. Attempt only once QEMU is fully working. It is safe: the kernel runs entirely in RAM and never touches the internal disk, so the existing Windows install is unaffected.

### Deliberately excluded

| Excluded | Reason |
|---|---|
| **Custom bootloader** | GRUB handles it. Writing our own teaches legacy BIOS trivia, not OS concepts. |
| **64-bit long mode** | Requires paging before first output. Same concepts, worse learning order. See §1.2. |
| **Filesystem / file manager** | Requires an ATA/AHCI disk driver plus a FAT32 implementation (~50–70h). Weak concept-per-hour ratio. |
| **GUI with mouse/windows** | Requires a USB host stack for input on modern hardware, plus a windowing system. 150h+. |
| **Web browser** | Requires, in order: USB stack → network driver → TCP/IP stack (~40k lines) → TLS (~100k lines) → HTTP → HTML/CSS/JS engines. Not achievable at any student scale; listed here because knowing *why* is the point. |
| **SMP / multi-core** | The 5600G has 12 threads, but SMP requires APIC, per-core stacks and locking throughout. Out of scope. |
| **Framebuffer graphics** | See §2.10. |

---

## 4. Technical reference

### Target
- **Emulator:** `qemu-system-i386`
- **Architecture:** i386, 32-bit protected mode
- **Bootloader:** GRUB 2 via Multiboot 1
- **Kernel load address:** `0x100000` (1 MB) — the Multiboot convention
- **Real hardware target:** AMD Ryzen 5 5600G (x86-64, runs 32-bit code natively)

### Multiboot header

Must appear within the **first 8 KB** of the kernel binary and be **4-byte aligned**, or GRUB refuses to load it:

| Field | Value |
|---|---|
| Magic | `0x1BADB002` |
| Flags | `0x00000003` (align modules, provide memory map) |
| Checksum | `-(magic + flags)` |

### Key I/O ports and addresses

| Device | Address / Port |
|---|---|
| VGA text buffer | `0xB8000` (memory-mapped, 80×25, 2 bytes per cell) |
| COM1 serial | `0x3F8` |
| PIC master | `0x20` (command), `0x21` (data) |
| PIC slave | `0xA0` (command), `0xA1` (data) |
| PIT | `0x40` (ch0 data), `0x43` (command) |
| PS/2 keyboard | `0x60` (data), `0x64` (status/command) |

VGA cell format: low byte = ASCII character, high byte = attribute (`background << 4 | foreground`). So `0x0F41` is a white-on-black `A`.

### Interrupt vector layout (after PIC remap)

| Vector | Source |
|---|---|
| 0–31 | CPU exceptions (0 = divide error, 13 = GPF, 14 = page fault) |
| 32 | IRQ 0 — **PIT timer** |
| 33 | IRQ 1 — **PS/2 keyboard** |
| 34–47 | IRQ 2–15 |
| 128 (`0x80`) | **System call** *(Tier 3)* |

**The PIC remap is not optional.** By default the PIC delivers IRQ 0 on vector 0, which is also the divide-by-zero exception — the two become indistinguishable. Remapping to 32–47 is the first thing to do after installing the IDT.

### Important control registers

| Register | Use |
|---|---|
| `CR0` | Bit 0 = protected mode; **bit 31 = paging enable** |
| `CR2` | **Faulting linear address** — written by the CPU on a page fault |
| `CR3` | Physical address of the page directory |
| `CR4` | **Bit 4 = PSE** (4 MB pages) |

`CR2` is the key verification tool: on a page fault the hardware records the offending address there. Printing it proves the fault is real, because we never assigned that value.

### Toolchain

| Tool | Purpose |
|---|---|
| **WSL2 (Ubuntu)** | Linux environment on Windows |
| **`gcc-multilib`** | Build 32-bit freestanding binaries with `gcc -m32 -ffreestanding -nostdlib -fno-pie -no-pie` |
| **`nasm`** | Assembler for `boot.S`, `isr.S`, `context.S` |
| **`qemu-system-x86`** | Emulator (`qemu-system-i386`) |
| **`grub-pc-bin`, `xorriso`** | `grub-mkrescue` — builds a bootable ISO |
| **`make`** | One command to build and launch |
| **`gdb`** | Attaches to QEMU's debug socket (`-s -S`) |

> **Toolchain note.** OSDev convention is to build a dedicated `i686-elf-gcc` cross-compiler, which takes 1–2 hours of compiling. For a 32-bit kernel, the host compiler with `-m32 -ffreestanding -nostdlib -fno-pie -no-pie` works in practice and saves that time. If unexplained linker or startup problems appear, building the proper cross-compiler is the fallback.

### Source layout

```
kernel/
  boot.S            # multiboot header, _start, stack, call kmain
  isr.S             # exception + IRQ assembly stubs
  context.S         # context switch
  kmain.c           # kernel entry point
  vga.c/.h          # 0xB8000 text output
  serial.c/.h       # COM1 debug channel
  gdt.c/.h          # segment descriptors
  idt.c/.h          # interrupt descriptor table
  pic.c/.h          # 8259 remap, EOI
  pit.c/.h          # timer
  keyboard.c/.h     # PS/2 scancode decoding
  task.c/.h         # task structs, scheduler
  heap.c/.h         # kmalloc / kfree
  sync.c/.h         # spinlock, mutex, semaphore
  shell.c           # command loop
  monitor.c         # `top` display
  selftest.c        # automated assertions
  demo.c            # scripted walkthrough
  syscall.c/.h      # int 0x80 dispatch          (Tier 3)
  paging.c/.h       # page directory, CR3        (Tier 3)
  linker.ld
grub.cfg
Makefile
```

---

## 5. Schedule

**10 days (12 → 22 August).** Tier 1 ≈ 16 h, Tier 2 ≈ 24 h, Tier 3 ≈ 15 h.

| Day | Date | Goal | Milestone |
|---|---|---|---|
| 1 | Aug 12 | WSL2 + toolchain + GRUB ISO pipeline | `make run` boots GRUB in QEMU |
| 2 | Aug 13 | Multiboot header, `boot.S`, `linker.ld`, VGA driver | **★ Text appears on screen** |
| 3 | Aug 14 | Serial debug channel, GDT | Kernel logs to file for post-mortem debugging |
| 4 | Aug 15 | IDT + ISR stubs + PIC remap | Faults print diagnostics instead of rebooting |
| 5 | Aug 16 | PIT timer + PS/2 keyboard | **★ Interrupt-driven input working** |
| 6 | Aug 17 | Context switch + scheduler + preemption | **★ Tasks interleaving; `preempt off` ablation** |
| 7 | Aug 18 | Heap allocator + mutex + race demo | `meminfo`, `race off`/`race on` |
| 8 | Aug 19 | Shell + `top` monitor + `demo` + `selftest` | Full command set; all self-tests passing |
| 9 | Aug 20 | **Tier 3** — syscalls, then paging, then USB boot | Stretch only |
| 10 | Aug 21 | README, GIF, technical report, final commit | Submission-ready |
| — | Aug 22 | **Deadline — buffer, no new code** | |

### Hard rules

1. **Day 2 gate.** If text is not on screen by end of day 2, stop and escalate. On x86 this is a one-line write to `0xB8000`; failing it means the *build or boot* is wrong, not the code, and more feature work will not help.
2. **Day 6 gate.** If tasks are not interleaving by end of day 6, cut Tier 3 entirely and spend the remaining days polishing Tier 2. A finished Tier 2 beats a broken Tier 3.
3. **No new code after day 10.** Aug 22 is buffer, not a work day.
4. **Tier 3 is optional by design.** Nothing in §6 "must have" depends on it.
5. **Log challenges the day they happen** into [`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md) §3. Same-day entries are visibly better than ones reconstructed on day 10.

### ⚠ Capacity warning

Tier 1 + Tier 2 is **~40 hours**. At 1 hour/day for 10 days, one person has **10 hours**. **The plan does not fit a single person at that rate.** Either the team is 3–4 people, or daily hours increase substantially, or scope is cut to Tier 1 plus scheduler only. **This must be resolved on day 1** — see §9.

---

## 6. Definition of done

### Must have
- [x] Boots under GRUB in QEMU without crashing
- [x] Prints a banner to VGA text mode
- [x] Serial debug logging to a file
- [x] IDT installed; CPU exceptions print diagnostics rather than triple-faulting
- [x] PIC remapped; PIT firing at a known frequency
- [x] Keyboard input working, interrupt-driven
- [x] **Preemptive** multitasking — ≥3 tasks, no voluntary yielding
- [x] `preempt off` ablation demonstrably breaks scheduling
- [x] `kmalloc`/`kfree` with `meminfo` output
- [x] Mutex — race demo fails without it, passes with it, across repeated runs
- [x] Shell with ≥8 working commands — 15 implemented
- [x] `top` monitor reading live kernel state
- [x] `demo` and `selftest` commands — 21 checks
- [x] `make run` works from a clean clone
- [x] README with build instructions, architecture notes, and screenshots
- [x] [`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md) complete

### Stretch
- [x] Ring 3 with `int 0x80` syscalls
- [x] Paging enabled; page fault reports `CR2` correctly
- [x] Semaphore-based producer/consumer (bounded buffer, 4 slots)
- [x] Command history (up/down arrows, 16 entries)
- [ ] **Booted on real hardware (5600G) from USB — not attempted, QEMU only**

### Presentation deliverable

The portfolio artefact is **not the repo** — it is a **30-second GIF at the top of the README**, recorded from the `demo` command:

> boot → `spawn 3` → interleaved output → `preempt off` (breaks) → `preempt on` (recovers) → `race off` (fails) → `race on` (passes) → `fault 0xDEADBEEF` → `selftest` (all PASS)

Record with [asciinema](https://asciinema.org) or OBS → GIF. If Tier 3 #21 succeeds, add a phone video of the real machine booting.

---

## 7. Working notes

- **Everyone needs WSL2.** You cannot review code you cannot build.
- **We type and understand every line.** Following a tutorial closely is legitimate and is what we are doing; cloning a finished kernel is not, and it defeats the assessment, which is whether we can explain the code.
- **Commit small and often.** When the kernel silently stops booting, `git diff` against the last working commit is the fastest debugging tool available.
- **Set up the serial debug channel early (day 3).** `-serial file:log.txt` in QEMU means that when the kernel triple-faults and reboots, the log survives. Without it, a crash loop erases all evidence.
- **A rebooting QEMU window means a triple fault** — an exception raised while handling an exception while handling an exception. It is the standard x86 beginner failure. `qemu -d int,cpu_reset` prints what actually happened.
- **Comment the assembly heavily.** Nobody, including its author, will remember what `isr.S` does in a week.
- **Learn GDB by day 4.** `qemu -s -S` plus `target remote :1234` turns silent hangs into inspectable state. It pays for itself immediately.

---

## 8. Primary references

| Resource | Use |
|---|---|
| [The Little Book About OS Development](https://littleosbook.github.io) | **Primary guide.** Exactly our stack: GRUB multiboot, i386, VGA text, IDT, PIT, keyboard, paging, ring 3 |
| [OSDev Wiki — Bare Bones](https://wiki.osdev.org/Bare_Bones) | Canonical minimal multiboot kernel; start here on day 2 |
| [OSDev Wiki — Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton) | Project structure once past bare bones |
| [OSDev Wiki](https://wiki.osdev.org) | Reference for PIC, PIT, PS/2, GDT, TSS, paging |
| [Intel SDM Vol. 3](https://software.intel.com/en-us/articles/intel-sdm) | Authoritative on protected mode, descriptors, exceptions |
| [osdev.org forums](https://forum.osdev.org) | Where to ask when stuck; search first, most beginner failures are already answered |

---

## 9. Open questions — resolve on day 1

- **Team size and realistic weekly hours per person.** See the capacity warning in §5. This determines whether the plan is Tier 1+2+3, Tier 1+2, or a reduced scope. **Nothing else can be scheduled honestly until this is answered.**
- Suggested split once known: one person on Tier 1 (toolchain, boot, VGA, IDT), one on scheduler + context switch, one on heap + sync + shell. Tier 1 blocks everyone, so pairing on it is worth considering.
- Is a written report required alongside the repo, or is [`TECHNICAL_REPORT.md`](TECHNICAL_REPORT.md) sufficient?
- Live demo required, or is a recording acceptable?
- Is Tier 3 expected, or is a polished Tier 2 acceptable for full marks?
