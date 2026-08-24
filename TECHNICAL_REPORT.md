# Technical Report — Challenges Faced

**Project:** TazOS, a bare-metal x86 operating system kernel
**Course:** Operating Systems
**Submitted:** *(date)*
**Authors:** *(names)*
**Repository:** *(URL)*

---

## 1

**Situation** — GRUB refused to boot the kernel, reporting `no multiboot header found`. The header was plainly present in my source, and its magic number and checksum both matched the specification, so the error described a condition I could not see.

**Task** — I was responsible for the boot pipeline and had to make the kernel loadable before any other work could begin.

**Action** — I had been re-reading the same source file against the specification and getting nowhere, so I changed what I was inspecting. Instead of the source, I examined the binary the linker actually produced. Running `objdump -h` showed `.text` sitting at file offset 8192, and searching the raw bytes with `od` put the multiboot magic value at that same offset. The specification requires the header within the first 8192 bytes, meaning offsets 0 to 8191 — it was one byte outside the window. I then traced why the section had moved and found a `.note.gnu.build-id` section that the compiler had inserted without being asked, occupying the first page and displacing everything behind it. I added `-Wl,--build-id=none` to the link flags and an explicit `/DISCARD/` block to the linker script covering the note sections and `.eh_frame`, so that no future auto-generated section could repeat the displacement.

**Result** — The kernel booted immediately, with `.text` at offset 4096. I added `grub-file --is-x86-multiboot` as a build step, so this failure is now caught automatically rather than debugged by hand.

---

## 2

**Situation** — Timer interrupts fired exactly once. The first tick arrived and the scheduler ran, after which no further interrupt was ever delivered and the system stopped responding to time.

**Task** — I had written the interrupt dispatcher and was responsible for making preemptive scheduling work on top of it.

**Action** — My dispatcher followed the conventional ordering that appears in most examples: call the registered handler first, then send the end-of-interrupt byte to the programmable interrupt controller. I checked the controller's initialisation and the interrupt descriptor table entry, both of which were correct, so instead I traced the execution path of a single tick by hand. That exposed the flaw. The ordering is correct only for handlers that return to their caller, and my timer handler ends in a context switch, which changes the stack pointer and resumes a different task. Control never came back, so the acknowledgement was unreachable. The controller was waiting for a byte that would never arrive and correctly refused to deliver another interrupt. I moved the acknowledgement to before the handler call, which is safe because the controller only needs to know the interrupt was received, not that the handler has finished with it.

**Result** — Timer interrupts arrive continuously and preemption works. The scheduler records over 250,000 context switches in a normal session.

---

## 3

**Situation** — The first task I launched into ring 3 raised a page fault on its very first instruction, before executing any of its own code.

**Task** — I was implementing privilege separation and needed unprivileged code to run at all before I could demonstrate that the processor restricts it.

**Action** — I had marked the pages holding the user program as user-accessible in the page table and assumed that was sufficient, since that is where the code lives. Because the fault occurred before the program's first instruction, I reasoned the problem had to lie in the transition rather than in the program, so I read the manual's description of address translation rather than my own code. The processor checks the user-accessible bit at *every* level of the page structure, not only the last, and the effective permission is the logical AND across those levels. My page table entry granted user access but the page directory entry above it did not, so the combination denied it. I set the user bit on the directory entry as well, then reviewed every other place where I had assumed a single-level permission check.

**Result** — Ring 3 tasks run correctly. The boot self-test now verifies both halves of the boundary on every start: a program that asks the kernel through the syscall gate completes, and one that touches a hardware port directly is killed by the processor.

---

## 4

**Situation** — Dragging any window consumed the entire processor. Measured over a ten-second drag, the idle task received zero timer ticks and the context-switch counter did not advance at all, meaning the rendering loop held the CPU for the whole operation.

**Task** — I had written the window manager and was responsible for making the desktop usable rather than merely functional.

**Action** — I instrumented before guessing, sampling the task list before and after a drag to establish that the loop genuinely monopolised the processor. Reading the render path, I found that a drag set the flag meaning "the scene changed", which caused every mouse packet to trigger a complete rebuild: the whole wallpaper redrawn through a palette lookup per pixel, every window re-rendered including all of its anti-aliased text, and an eight-megabyte copy to the screen. At 1920x1080 with four bytes per pixel, that is roughly 1.6 gigabytes per second of memory traffic to move one rectangle. I recognised that none of the work was necessary, because a window's contents do not change while it is dragged — only its position does. I rewrote the path to rasterise the window once when the drag begins, storing its pixels in a heap buffer, then on each step to repaint only the strip the window uncovered and copy the cached pixels to the new position. This required extending the wallpaper renderer to draw an arbitrary sub-rectangle, which it previously could not do.

**Result** — The idle task went from zero per cent of ticks to thirty-three, measured identically before and after. Input and background tasks are scheduled normally throughout a drag.

---

## 5

**Situation** — Moving a window is a rectangular block copy, which is precisely the operation a 2D graphics engine exists to accelerate. The adapter advertised the capability and my driver already implemented the command, but no code path had ever called it.

**Task** — I was looking for further performance in the window manager after the redraw work described in challenge 4.

**Action** — I routed the drag through the adapter's block-copy command, keeping the software back buffer synchronised because every later partial repaint reads from it. I then treated the change as a hypothesis rather than an improvement and measured it: the same build, the same adapter and the same ten-second drag, run with the feature enabled and disabled. The accelerated version was consistently slower — thirty-three per cent idle without it against twenty-seven to thirty with it. Investigating why, I found two reasons the trade does not pay under emulation. The emulator performs the block copy on the host processor, so no work is genuinely offloaded. And the command queue runs asynchronously, so before the processor may write the uncovered strips into video memory it must wait for the adapter to finish reading the region it is moving; discovering that requires polling a hardware register over an I/O port, which costs a virtual-machine exit on every read, once per frame. I kept the implementation but disabled it by default, recording the measurement in a comment beside the flag.

**Result** — Performance stayed at the better figure. The decision is documented with the numbers that produced it, so it can be re-tested on hardware where the copy is genuinely offloaded.

---

## 6

**Situation** — Three background worker threads appeared to consume the whole machine. The task list showed them accumulating processor time rapidly, while the idle task showed almost none.

**Task** — I had written the workers as a load generator, to demonstrate that several threads can run while the desktop stays responsive.

**Action** — Before changing the workers I questioned whether the measurement itself was trustworthy, because the desktop did not feel as loaded as the numbers claimed. Each worker incremented a counter, yielded, then executed `hlt` to wait for the next interrupt. Reading my own accounting code, I found the flaw was in the reporting rather than the workload: a halted task is still the *current* task, so when the timer interrupt fired it charged that worker for time it had spent doing nothing at all. The threads were mostly idle and being billed as busy. I replaced the halt with a proper sleep, which marks the task blocked and immediately reschedules, so the idle task genuinely receives the time. I also gave the workers real arithmetic to perform in bounded batches, so that the ticks they are charged represent work they actually did. Tuning the batch size took two attempts, as the first was small enough to complete inside a single timer tick, which made the workers invisible in the task list entirely.

**Result** — Three workers now consume roughly thirteen ticks per second between them, and the idle task holds around forty per cent with all three running. The reported figures match observable behaviour.

---

## 7

**Situation** — My kernel implemented system calls correctly, with a single interrupt gate at privilege level 3 and a ring 3 program that the processor terminates when it accesses hardware directly. However, the trap instruction appeared on only two lines in the entire source tree, both inside demonstration code, so nothing a user of the desktop could do would ever produce a system call.

**Task** — I needed the system call interface to be part of how the system works, rather than a feature shown in isolation.

**Action** — I first established the actual position rather than assuming it, searching the tree for every occurrence of the trap instruction and every caller of the function that enters ring 3. Both confirmed that the interface was exercised only by the demonstration. Every application on the desktop is compiled into the kernel and runs at ring 0, so saving a file from the editor called the filesystem directly, as an ordinary function call. Rebuilding all of them as user-space processes would require a process model, separate binaries and a program loader, which was beyond the scope of this project. Instead I routed one genuine operation through the interface: saving a file now places the call number in `eax` and the filename, buffer and length in `ebx`, `ecx` and `edx`, then executes `int 0x80`, and the kernel's dispatcher performs the write. To make the change verifiable rather than merely asserted, I added a counter that increments only inside the interrupt handler, together with a command that prints it.

**Result** — Saving a file from the editor now genuinely traps into the kernel, verified with the desktop running and a disk attached: seven calls serviced before the save and eight immediately after, with the file present on disk at the correct size.
