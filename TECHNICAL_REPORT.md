# Technical Report — Challenges Faced

**Project:** TazOS, a bare-metal x86 operating system kernel  
**Course:** Operating Systems  
**Submitted:** *(date)*  
**Authors:** *(names)*  
**Repository:** *(URL)*

---

## 1. GRUB refused to load the kernel: header one byte out of range

**Situation.** The kernel compiled and linked cleanly, but GRUB rejected it with `no multiboot header found`. The header struct was plainly present in the source and the magic constant was correct, so the error gave no indication of what was actually wrong.

**Task.** We had written the multiboot header as an assembly section and assumed that any section defined in the source would appear near the start of the output binary. Debugging began by re-checking the magic value and the checksum arithmetic against the Multiboot specification — all of which were correct.

**Action.** We stopped reading the source and inspected the produced binary instead. `objdump -h` showed `.text` at file offset `0x2000`, and searching the raw bytes with `od` located the magic value at **file offset 8192** — while GRUB only scans the first 8192 bytes, offsets 0 to 8191. The header was one byte outside the search window.

The cause was a section we had not written: the linker was placing `.note.gnu.build-id` first, at `0x100000`, where it consumed the first page and pushed `.text` (and with it the multiboot header) onto the next one. We added `-Wl,--build-id=none` to the link flags and an explicit `/DISCARD/` block in the linker script covering the note sections and `.eh_frame`.

**Result.** GRUB loaded the kernel on the next attempt, with `.text` at file offset 4096, comfortably inside the window. `grub-file --is-x86-multiboot` was added to the build as a hard check so this class of failure can never again reach the point of being debugged interactively. More broadly, this established inspecting the binary rather than the source as the first debugging step, which paid off repeatedly.

---

## 2. Two source files silently overwriting each other's object file

**Situation.** After adding interrupt handling, the link failed with `undefined reference to isr_handler` — for a function that plainly existed and had compiled without error. Simultaneously it reported `multiple definition of irq15`, pointing at the same file as both the duplicate and the original.

**Task.** The Makefile derived object names by pattern substitution, mapping `kernel/%.c` and `kernel/%.asm` onto `build/%.o`.

**Action.** The contradictory pair of errors — a symbol both missing and duplicated — pointed at the build rather than the code. `kernel/isr.c` and `kernel/isr.asm` both mapped to `build/isr.o`. Whichever compiled last overwrote the other, so the C symbols and the assembly symbols could never both be present. We changed assembly objects to a `.asm.o` suffix.

**Result.** The link succeeded. The suffix also pre-empts the same collision for the `context.c`/`context.asm` pair added later, which would otherwise have reproduced the bug in a subtler form once the scheduler existed.

---

## 3. Acknowledging an interrupt after a handler that never returns

**Situation.** Anticipated rather than observed. While writing the scheduler it became clear that the timer would stop firing permanently on the first switch to a newly created task.

**Task.** The IRQ dispatcher followed the conventional shape: call the registered handler, then send the end-of-interrupt byte to the PIC.

**Action.** The timer handler drives the scheduler, and a context switch does not return — it swaps stacks and resumes a different task. If that task was newly created it has never been inside the dispatcher, so an EOI placed *after* the handler call would simply never execute. The PIC would continue to believe IRQ 0 was in service and deliver no further timer interrupts: the scheduler would run exactly once and the machine would freeze with no diagnostic. We moved the EOI to before the handler call, which is safe because interrupt gates clear the interrupt flag on entry, so no nested interrupt can arrive in the interim.

**Result.** Preemption worked on the first attempt. This is the one significant problem in the project that was reasoned about in advance rather than discovered by debugging, and the contrast is instructive: it would have presented as a total freeze with no output, which is among the hardest symptoms to work backwards from.

---

## 4. Format specifiers printed literally, corrupting every argument after them

**Situation.** The per-task CPU time table rendered as `%-10s id=1089572 state=%-8s ticks=1`, with the format specifiers appearing verbatim and the task IDs showing implausible values.

**Task.** `kprintf` had been written to handle a bare conversion character plus an optional zero-padded width, which was all the boot messages had needed.

**Action.** The specifiers were being echoed because `%-10s` was unsupported, but the more serious effect was invisible: the parser consumed the wrong number of variadic arguments, so every subsequent value in the call was read from the wrong stack slot. The implausible IDs were the *name pointers* being printed as integers. We rewrote the formatter to render each field into a buffer first and then apply width, alignment and fill separately, adding support for `-` and space fill, and taking care that a minus sign precedes zero padding rather than being buried inside it.

**Result.** Tables render correctly, which the kernel monitor depends on entirely. The instructive part is that the visible symptom (literal `%-10s`) was cosmetic while the invisible one (misaligned varargs) was producing confidently wrong numbers — output that looked like data.

---

## 5. The race condition demo produced the correct answer

**Situation.** The self-test asserted that two tasks incrementing a shared counter without a lock would lose updates. Every run returned exactly the expected total, so the demonstration proved nothing.

**Task.** Each racer performed a read, a short fixed-length delay loop, and a write, repeated several thousand times.

**Action.** The window between read and write was far too narrow relative to the timeslice: at 50 ms per slice and a critical section lasting a microsecond or two, the chance of preemption landing between the read and the write was roughly one in twenty-five thousand per iteration. A fixed spin count is the wrong instrument regardless, because how long it takes depends entirely on the host CPU.

We tied the width of the window to the same clock that drives preemption instead. The kernel now measures at runtime how many spin iterations fit inside one timer tick, then holds each update open for a randomised fraction of that. The timeslice was also shortened from five ticks to one.

**Result.** Updates are now reliably lost, and — after a further correction, below — by a varying amount.

---

## 6. The same demo then became *too* reliable

**Situation.** Having made the race fire, the total pinned to exactly half the expected value on every run. The self-test's separate assertion that results vary between runs began failing, on roughly one boot in three.

**Task.** Each iteration waited for the tick counter to change before writing back.

**Action.** Waiting on the clock edge made both racers wait on the *same* edge. They fell into lockstep, lost an update on every single iteration, and produced a result reproducible to the digit. That still demonstrates a race, but a number identical on every run is indistinguishable from a hardcoded one, which undermines the reason for showing it.

The window was narrowed to a randomised span of up to a quarter of a tick's work. The quarter matters: the calibration measures spinning while running alone, but two racers share the CPU, so the same work occupies roughly twice the wall-clock time — a window sized at a full tick again exceeds a timeslice and returns the loss probability to near certainty.

**Result.** Totals now scatter — `93 78 84 63 89` across five runs — with the locked variant exact every time. Verified stable across four consecutive boots. The lesson is that a demonstration can fail by being too deterministic as easily as by not firing at all.

---

## 7. A stale object file compiled against an old constant

**Situation.** After reducing a constant in `demos.h` from 8000 to 100, the self-test continued printing `expected 8000` while the demo itself used the new value. Source and binary disagreed with no warning from anywhere.

**Task.** The Makefile declared object files as depending on their `.c` file only.

**Action.** `selftest.c` had not itself changed, so make saw no reason to rebuild it, and `selftest.o` remained linked against the previous value of a macro defined in a header. We added `-MMD -MP` to the compile flags, which emit a dependency file per object listing every header it included, and an `-include` of those files in the Makefile.

**Result.** Editing a header now rebuilds everything that uses it. This is ordinary practice in any C project, but its absence is unusually confusing here because the failure presents as code that appears to ignore its own source.

---

## 8. The compiler deleted a deliberate divide-by-zero

**Situation.** The `fault div0` command reported `still alive - the fault did not fire`. No exception was raised, and the task ran to completion.

**Task.** The fault was written in C as a division by a variable holding zero, marked `volatile` specifically so the compiler could not fold the operation away.

**Action.** Disassembling the object file showed **no division instruction at all**. gcc had emitted a comparison and a conditional move: `1 / x` is nonzero only for `x` equal to 1 or -1, and division by zero is undefined behaviour, so the compiler is entitled to assume the case never occurs and rewrite the expression as a branchless select. `volatile` had forced the *load* of the operand, not the division that consumed it.

The instruction was written directly in inline assembly, leaving the compiler nothing to reason about.

**Result.** All three synchronous fault demos now raise genuine exceptions and are contained: the faulting task is killed and the shell survives. The general lesson is sharper than the specific fix — **you cannot reliably provoke a CPU exception from C**, because every way of doing so is undefined behaviour, and undefined behaviour is exactly what an optimiser is licensed to assume away. The same reasoning was applied pre-emptively to the null-pointer demo added later.

---

## 9. Ring 3 faulted on its first instruction: permissions are an AND across levels

**Situation.** The transition into user mode succeeded — the saved `CS` read `0x1B`, confirming ring 3 — but the very first instruction fetch raised a page fault with error code 5: present, read, user mode. A protection violation rather than a missing page.

**Task.** The paging code set the `PAGE_USER` flag on every page-table entry covering the first 4 MB, where both the user code and its stack reside.

**Action.** The error code was the clue: *present* meant the mapping existed, so the address was fine and the permission was not. The CPU computes the effective permission as the logical AND of every level of the page-table walk, and the page *directory* entry pointing at that table had only present and write bits. A user-accessible page reached through a kernel-only directory entry is still kernel-only. `PAGE_USER` was added to the directory entry.

**Result.** Ring 3 code executes. It is worth noting explicitly that this fix makes the entire first 4 MB user-accessible, which is why memory isolation is listed as a known limitation in §1.3 rather than claimed as working — instruction privilege is enforced, memory separation is not.

---

## 10. Every system call reported as an unknown exception

**Situation.** With ring 3 running, `int 0x80` reached the kernel but was reported as `CPU EXCEPTION 128: unknown`, and the calling task was killed as though it had faulted.

**Task.** The exception dispatcher looked up a registered handler before falling through to its diagnostic path.

**Action.** The lookup was written as `if (n < 32 && handlers[n])`, from when the only registered handlers were CPU exceptions. The syscall vector is 128, so the guard excluded it and the correctly-registered dispatcher was never consulted. Removing the bound fixed it.

**Result.** System calls dispatch correctly. The bug is trivial; what makes it worth recording is that it was introduced by an assumption that was true when written and quietly became false — the guard encoded "handlers are only ever exceptions", which no longer held once a syscall gate existed.

---

## 11. The monitor scrolled instead of redrawing

**Situation.** The live `top` display was intended to refresh in place. On screen it printed `[H` literally and scrolled, stacking successive frames down the display.

**Task.** The monitor emitted the ANSI escape `\033[H` through `kprintf` to home the cursor between frames.

**Action.** ANSI escapes are a terminal convention. The serial side is read by a terminal and honours them; the VGA side is a memory-mapped grid of character cells with no notion of escape sequences, so it faithfully rendered the bytes. We added a `console_home()` that moves the VGA hardware cursor directly and emits the escape only to the serial channel.

**Result.** The display refreshes in place on both channels. The underlying mistake was treating two genuinely different devices as one because they share an interface.

---

## 12. A demo that overstated its own evidence

**Situation.** The `race` command printed "totals differ from each other and from the expected value" whenever any run was incorrect — including runs where all three totals were identical, which a screenshot captured plainly.

**Task.** The summary branched on a single flag recording whether every run had returned the expected total.

**Action.** The message was making a claim the code had not checked. We added a second flag tracking whether the results actually differed from one another, and split the summary into three honest cases: no updates lost, updates lost with varying totals, and updates lost with identical totals.

**Result.** The command now reports what happened rather than what usually happens. This is a small fix, but the failure mode is worth naming: the output was *arguing for the correctness of the system using evidence that was not on screen*. That is precisely the kind of claim a reader should distrust, and the reason the verification approach in §4.3 avoids relying on narration.

---

## 13. A synchronisation demo revealed the console was itself unsynchronised

**Situation.** The newly added producer/consumer demonstration printed a marker per item. Its output came out as `c14 cP16 P17 P18 15` — one task's `c15` had been split down the middle, with another task's three markers wedged inside it.

**Task.** `kprintf` wrote straight through to the VGA buffer and the serial port with nothing serialising it. Every other shared structure in the kernel — the heap free list, the task list, the bounded buffer — was already protected, but the console had never been thought of as shared state.

**Action.** The splicing was correct behaviour for unsynchronised code: a task preempted partway through a call resumes later, and whatever ran in between wrote to the same screen. The obvious fix is a mutex around `kprintf`, and it is the wrong one. Fault handlers print, and a fault can occur *inside* a `kprintf` — the handler would then block on a lock held by the very task it interrupted, deadlocking against itself with no way out.

We added a nesting `preempt_disable()` / `preempt_enable()` counter to the scheduler instead, and wrapped `kprintf` in it. Interrupts stay enabled throughout, so the timer keeps ticking and CPU time is still charged to the task; only the reschedule is deferred. A counter nests where a lock deadlocks.

**Result.** Output is atomic per call: the demo now reads `P1 P2 P3 P4 c1 c2 c3 c4 P5 …`, which also makes the semaphore's four-slot bound visible directly rather than buried in noise.

Two things make this worth recording. First, the bug was found by a feature rather than by a test — writing the producer/consumer demo surfaced a defect in code that had been running since the first day. Second, it is a small lesson in what counts as shared state: the console had been treated as an output *service* rather than as a resource two tasks could contend for, and that framing is what hid it.

---

## 14. A polling loop that could hang the kernel outside the emulator

**Situation.** Preparing to run the kernel in VMware rather than only in QEMU, we reviewed the code for assumptions that hold in an emulator but not elsewhere. The serial driver waited in an unbounded loop for the UART to report its transmit register free.

**Task.** The driver assumed a serial port exists, because QEMU always provides one. Every message the kernel prints passes through that function.

**Action.** VMware gives a guest no serial port unless one is configured, and most modern PCs have none at all. Reading an absent port returns whatever the bus floats to — commonly `0xFF`, which coincidentally has the ready bit set, but `0x00` on some chipsets, which reads as permanently busy. In that case the loop never exits and the kernel hangs inside its first `kprintf`, before anything reaches the screen: a black display with no diagnostic, the hardest possible symptom to work backwards from.

We added a UART loopback probe at startup — put the chip in loopback, write a byte, check the same byte returns — and bounded the transmit wait. A missing port now disables serial output rather than blocking on it.

**Result.** The kernel boots identically with and without a UART, verified both ways under QEMU: 30 self-tests pass and the screen output is identical. Dropping debug output when the hardware is absent is survivable; hanging to deliver it is not.

The method is worth noting as much as the fix. Rather than testing the same configuration again, we asked what QEMU was being *forgiving* about — which is where the defects that only appear on real hardware live.

---

## 15. Toolchain and workflow

**Situation.** The project began with no command-line experience on the team: no terminal use, no compiling from a shell, no Git CLI, and no debugger. Bare-metal development requires all of these and provides none of the feedback an IDE gives.

**Task.** *(Fill in: how were you working before — GitHub Desktop, IDE run buttons, no build system?)*

**Action.** The build environment was moved into a Docker image containing the whole toolchain — cross-compiler, assembler, QEMU, GRUB, ISO tools and debugger — so that nothing had to be installed on the host and every machine gets an identical setup. *(Fill in what you personally did: running the build, reading errors, using git.)*

**Result.** *(Fill in.)* Worth noting: the single most valuable piece of infrastructure turned out to be serial logging to a file, because it is the only thing that survives a triple fault and reboot. It was set up on day one rather than after it was first needed.

---

## 16. A window manager that redrew eight megabytes to move a mouse

**Situation.** The first working desktop was unusable. The pointer lagged several centimetres behind the mouse, and typing produced characters a visible moment after the key.

**Task.** The render loop began each frame by filling the whole screen, then redrew the wallpaper, every window and the cursor, before copying the entire back buffer forward.

**Action.** At 1920x1080 and four bytes per pixel a frame is 8.3 megabytes. Redrawing and then copying that on every mouse interrupt is roughly 1.6 gigabytes a second of memory traffic to move a pointer a few pixels. The machine was not slow; it was being asked to do an enormous amount of pointless work.

Two changes fixed it. The scene is now rebuilt only when something in it actually changed, tracked by a separate `scene_dirty` flag from the per-frame `dirty` rectangle, and a mouse movement alone does not qualify. And the cursor no longer forces a redraw at all: the pixels underneath it are saved before it is drawn and put back before it moves, so erasing it costs one small rectangle rather than a whole frame.

**Result.** Pointer movement now touches two cursor-sized rectangles instead of the screen. The desktop is responsive at 1920x1080, and the lesson generalises: the expensive operation was not drawing, it was drawing things that had not changed.

---

## 17. A cursor that flickered because the frame was correct twice

**Situation.** With the redraw fixed, the pointer left a faint flicker along its path.

**Task.** The loop erased the cursor from its old position, presented, drew it at the new position, and presented again.

**Action.** Presenting between the erase and the redraw seemed tidier, because it keeps each dirty rectangle small when the pointer jumps a long way. But it puts a frame on the screen in which the cursor does not exist. At sixty frames a second that intermediate frame is displayed long enough to be seen, and a cursor that vanishes and reappears reads as a flicker.

The fix was to remove work rather than add it: erase, redraw, and present once, covering both positions in a single rectangle.

**Result.** No flicker. Each frame is occasionally a slightly larger copy and is never in a half-drawn state. A frame that is individually correct can still be wrong to display, which is not obvious until it is on a screen.

---

## 18. A command that appeared to hang, and one that appeared to do nothing

**Situation.** Two complaints about the graphical terminal arrived together: typing `demo` froze the window until the demonstration finished, and at the very first prompt the characters being typed did not appear at all.

**Task.** Both symptoms came from the same place, when output reaches the screen, but from opposite ends of it.

**Action.** The shell's dispatch runs synchronously inside the render loop, so a command that takes seconds blocks every frame in between. Its output was accumulating in the terminal buffer and arriving all at once at the end, which is indistinguishable from a hang. The terminal now flushes and presents on every newline, so a long-running command shows its output as it produces it.

The invisible typing was the same mechanism inverted: the framebuffer console only presented on a newline, so a line being typed sat in the back buffer, correct and unseen, until Enter was pressed. It now presents on every character.

**Result.** Long commands visibly progress and typed characters appear as they are typed. Worth recording: neither was a bug in the code that produced the output. Both were about when the back buffer reached the screen, and neither would have been found by reading the shell.

---

## 19. A performance fix that broke the keyboard

**Situation.** Immediately after splitting the redraw into `scene_dirty` and `dirty`, the keyboard became laggy. A keystroke appeared only when the mouse was moved or the clock ticked.

**Task.** The split had been made to stop the mouse forcing a full redraw. The keyboard handler was updated at the same time.

**Action.** The handler set `dirty` but not `scene_dirty`. Under the old single-flag loop that had been enough; under the new one it asked for a present without asking for the scene to be rebuilt, so the character was never drawn. It appeared later only because some other event set `scene_dirty` and redrew everything, including the new text.

**Result.** One added line. The point is the shape of the mistake rather than its size: an optimisation that splits one piece of state into two silently changes the meaning of every existing write to it, and the compiler cannot help, because both names still exist and both still type-check.

---

## 20. A mouse with no scroll wheel

**Situation.** Scrolling the terminal with the mouse wheel did nothing, although scrolling by keyboard worked.

**Task.** The PS/2 mouse driver decoded the standard three-byte packet: buttons and flags, then relative X, then relative Y. There is no wheel field in it.

**Action.** The wheel is not part of the original PS/2 mouse protocol, and there is no command that asks for it. The extension is unlocked by a knock: set the sample rate to 200, then 100, then 80, in exactly that order. A mouse that recognises the sequence begins reporting device ID 3 instead of 0 and starts sending a fourth byte whose low nibble is a signed four-bit wheel movement. One that does not recognise it simply stays in the three-byte protocol, which is what makes the sequence safe to attempt blindly.

The driver performs the knock, asks for the device ID, and sets its packet size to four only if the answer is 3. The size has to be settled before any packet is decoded, or the byte stream desynchronises and the cursor moves in the wrong axis.

**Result.** The wheel scrolls the terminal's scrollback. This is a good example of a hardware interface that cannot be derived from first principles: no amount of reasoning about the three-byte packet produces the idea of setting the sample rate three times. It can only come from the documentation.

---

## 21. A filesystem whose every allocation returned null

**Situation.** The filesystem was added and immediately failed. No file could be created, and the two pre-seeded files were absent.

**Task.** Its initialiser had been placed in the bring-up sequence next to the other device initialisers, before the heap was set up.

**Action.** Files store their contents in heap memory, so the initialiser calls `kmalloc`, and `kmalloc` before `heap_init` has no memory to hand out and correctly returns null every time. The failure was not in the filesystem at all. It was the order of two lines in `kmain`.

The initialisation order in `kmain` is a dependency graph written out as a sequence, and it now says so in a comment at each point where the order is load-bearing rather than incidental: the filesystem after the heap because it allocates, and after the real-time clock because it timestamps.

**Result.** The filesystem works. The general lesson is that a kernel has no runtime to catch this. There is no module system and no initialisation-order checking; a subsystem used before it is ready does not raise an error, it returns zeroes.

---

## 22. Capability bits read against the wrong revision of a specification

**Situation.** The graphics adapter reported a capability word of `0x03`, and the driver concluded it supported neither of the two features it had checked for.

**Task.** The driver tested for the cursor and extended-FIFO capabilities using bit values taken from a published register reference.

**Action.** The values used, `0x10` and `0x20`, are from a later revision of the specification. In the legacy layout this adapter implements, those two capabilities are bits `0x01` and `0x02`, which are exactly the two bits that were set. The driver was reading a correct answer and rejecting it, because a hardware register only means something relative to the version of the interface it belongs to.

**Result.** The correct bits identify both capabilities, and the extended FIFO is used. The broader observation is that this class of defect reads as a hardware limitation rather than a software one. The adapter appeared not to support the features, and that is a conclusion which invites working around it instead of rechecking the constant.

---

## 23. A line-padding loop that became an infinite loop

**Situation.** Two symptoms were reported that sounded unrelated. In the text console, running `demo` made the screen scroll continuously and never stop. In the graphical desktop, the same command froze the display with no mouse pointer.

**Task.** The live monitor draws a box with a right-hand border, and every row is padded out to the box width before that border is printed. The padding was measured rather than counted by hand, which had been a deliberate fix for an earlier defect where three format strings that were supposed to be the same width were not. It measured by asking the VGA driver for its cursor column.

**Action.** The serial log settled it in one look: it was 92 KB, of which 87 KB was a single line consisting of one monitor row followed by eighty-seven thousand spaces.

Asking `vga_get_x()` was correct when it was written, because output went to the VGA text buffer. It stopped being correct when the framebuffer console was added. From then on the console routed characters to the framebuffer, or to a window, and the text-mode cursor never moved again — so the padding loop compared a fixed number against a limit it could never reach and emitted spaces for ever. Both symptoms were that one loop: in the console the spaces scrolled the screen, and in the desktop the render loop never got control back, so nothing repainted and the pointer was never redrawn.

The column is now tracked by the console itself, which is the layer that knows where characters actually went, and the loop is bounded so that padding a line cannot wedge the kernel even if the count is wrong again.

**Result.** `demo` and `top` both complete in the console and in the desktop, and the serial log for the same session is 10 KB with a longest line of 86 characters.

Two things are worth taking from it. The first is that the original fix was right and still broke: measuring the cursor instead of counting by hand was the correct instinct, but it hard-wired an assumption about which device was receiving output, and that assumption expired silently when a new one was added. The second is that an unbounded loop over a condition another subsystem controls is a hang waiting for a change somewhere else — the guard costs one comparison.

---

## 24. A demonstration that could not be watched

**Situation.** With the hang fixed, the multitasking step of the guided demo still arrived all at once in the graphical terminal: nothing, then thirty characters together.

**Task.** The terminal repainted on newlines, which had been added so that a long-running command showed progress instead of looking hung.

**Action.** The multitasking demonstration prints one character per task and no newline until every task has finished. Its entire point is watching three tasks interleave, and it was the one command whose output the newline rule could not show. The repaint now also triggers when enough time has passed since the last one, which covers output that contains no newlines without repainting on every character — at this window size that would cost more than the output is worth.

**Result.** The characters appear as the tasks produce them. Measured across the demo, consecutive frames now differ; before the change they were pixel-identical.

---

## 25. Dragging a window took the entire processor

**Situation.** Dragging any window pinned the CPU. It was not subtle: over a ten-second drag the idle task received zero ticks and the context-switch counter did not move at all, which means the render loop had the machine to itself for the whole drag.

**Task.** A drag set the flag that means "the scene changed", and the render loop treated that the same way it treats any other change. Every mouse packet therefore paid for a full rebuild: the entire wallpaper at two million palette lookups, a re-render of every window including all of the anti-aliased text inside them, and an eight-megabyte copy to the screen.

**Action.** None of that work was necessary, because the contents of a window do not change while it is being dragged. Only its position does. So the window is now rasterised once, when the drag starts, into a buffer taken from the heap. Each step of the drag restores the strip the window uncovered, blits the cached pixels at the new position, and copies the two affected rectangles forward. That is what a compositor does, and it needed the wallpaper to learn how to draw a sub-rectangle rather than only the whole desktop.

**Result.** The idle task went from zero per cent of the ticks to thirty-three, measured the same way before and after. The scheduler runs again during a drag, so input and background work are no longer starved.

Two bugs on the way there were worth as much as the fix. The first version pushed hand-picked rectangles to the screen and left a trail of ghost pointers, because erasing the cursor cleans the back buffer where the pointer used to be and that rectangle was in none of the ones being copied — letting the existing dirty-region tracker answer the question was both shorter and correct by construction. The second captured the window's pixels from its current position, which is the *destination*: input is handled earlier in the same loop iteration, so the back buffer there still held whatever was behind the window. It cached wallpaper and a strip of the next window along and blitted that as though it were the window being dragged.

---

## 26. The graphics adapter made the drag slower, not faster

**Situation.** Moving a window is a block copy, which is exactly what a 2D graphics engine is for. The adapter advertises the capability and the driver had a `RECT_COPY` command written for it. Nothing had ever called it.

**Task.** All window movement was done by the CPU, copying pixels into video memory by hand.

**Action.** We wired the drag path to hand the move to the adapter when it offers the capability, keeping the back buffer in step because every later partial repaint reads from it. Then we measured it against the same build with the flag off, on the same emulated adapter, over the same ten-second drag.

**Result.** It lost, repeatably: thirty-three per cent idle with the acceleration off, twenty-seven to thirty with it on.

Neither half of the trade pays under emulation. The emulator executes `RECT_COPY` on the host processor, so there is no blit engine being saved; and the ordering it forces costs more than the copy did, because the command queue runs asynchronously and the CPU may not write the uncovered strips into video memory until the adapter has finished reading the rectangle it is moving. Finding that out means polling a register over an I/O port, which is a virtual-machine exit on every read, once a frame.

The code stayed, defaulted off, with the measurement written into the comment beside the flag. On a VMware guest the same command is backed by the host's own graphics hardware, where the blit genuinely is free and the trade may well go the other way — but that is a measurement to take on the machine it matters for, not an assumption to inherit from this one. A negative result that is kept and documented is worth more than an optimisation nobody checked.

---

## 27. Three background workers appeared to eat the whole machine

**Situation.** Starting three background worker threads showed them accumulating CPU time fast enough to look as though they owned the processor, while the idle task starved.

**Task.** Each worker incremented a counter, yielded, and then executed `hlt` to wait for the next timer interrupt.

**Action.** The measurement was wrong before the workers were. A halted task is still the *current* task, so when the timer interrupt fired it charged that worker for time it had spent doing nothing at all. They looked like load and were mostly idle.

They now do a bounded batch of genuine arithmetic — a checksum, so the ticks they are charged are ticks they actually earned — and then call `task_sleep`, which marks them blocked and reschedules. The idle task genuinely runs, and the numbers in the task list mean what they say.

**Result.** Three workers now take about thirteen ticks a second between them rather than appearing to take the machine, and the idle task holds around forty per cent with all three running.

The tuning took two passes and both are worth recording. At a twenty-thousand-iteration batch the workers earned *zero* ticks in three seconds: each burst finished well inside a single timer tick, so they were never the running task when the interrupt arrived, and the task list showed nothing climbing at all. At eight million with a twenty-tick sleep all three climb steadily. That also changed what the demonstration can claim — a fixed batch and a fixed sleep produce near-identical shares, so the honest line is that the scheduler divides time *fairly*, not that the three numbers differ.

---

## 28. A window sank behind the panels a second after being clicked

**Situation.** Clicking Notepad brought it to the front, and roughly a second later it dropped behind Task Manager and Kernel Lab on its own, with nobody touching the mouse. Clicking again repeated the cycle, so the window could not be used.

**Task.** The desktop has a cheap repaint path for the windows whose contents change on their own — the task list, the statistics panel, the lab. Once a second it redraws those and copies them to the screen, instead of rebuilding the whole desktop.

**Action.** Windows are painted in array order, later ones on top. Redrawing only the live panels therefore painted them over anything above them that overlapped — which is exactly what Notepad was. The "a second later" was the refresh interval, and nothing was reopening: the window was being buried.

The path now marks the live panels, then marks any window above them that overlaps one, and paints that set in z-order.

**Result.** Notepad stays where it is put. Verified by opening it over both panels and leaving it for three and a half seconds — several refreshes, well past the point where it used to sink.

The general shape of the mistake is worth keeping: a partial repaint is an assertion that nothing else needs redrawing, and that assertion has to account for what is *in front of* the thing being repainted, not only the thing itself.

---

## 29. The system call interface existed and nothing used it

**Situation.** The kernel implements system calls properly — one interrupt gate with a descriptor privilege level of 3, four calls behind it, and a ring 3 program that the processor kills when it reaches for a hardware port. But `int 0x80` appeared on exactly two lines in the whole source tree, both inside the demonstration code. Nothing a user of the desktop could do would ever produce a system call.

**Task.** Every application on the desktop — the editor, the file manager, the task list — is compiled into the kernel and runs at ring 0. Saving a file from the editor called the filesystem directly, as an ordinary C function call.

**Action.** That is a defensible architecture for a kernel this size, but it made "the system uses system calls" a statement about the diagram rather than about the code. Rather than claim it, we made it true for one real operation: saving a file now loads the call number into `eax` and the name, buffer and length into `ebx`, `ecx` and `edx`, and executes `int 0x80`. The dispatcher gained a write-file call and reaches the filesystem from there.

We also added a counter that only increments inside the interrupt handler, and a command that prints it, so the trap can be demonstrated rather than asserted.

**Result.** Saving a file from the editor now genuinely traps into the kernel. Verified end to end with the graphical desktop running and a disk attached: seven calls serviced at the prompt, eight immediately after pressing Save, and the file present in the listing at the right size with that run's timestamp.

The scope is stated rather than blurred. One service is routed through the gate, not the whole desktop; the applications are still kernel code and could still call the filesystem directly, they simply no longer do. Making all of them user programs would require a process model, user-space binaries and a loader, which was outside what this project set out to build.

---

## 30. A script that only broke when it was read aloud

**Situation.** The presentation script for the project demonstration was written, reviewed and looked correct. Walking through it in front of the running system, two lines did not survive contact.

**Task.** The script had been written by reading the source and reasoning about what the interface would show.

**Action.** We ran the whole walkthrough against the built system instead of reading it. The script told the presenter to point at a "stack" column that does not exist in the graphical task list — that is the text-mode command; the window labels the same figure "Memory" and prints it in kilobytes. And it placed the line "the busiest thing is the idle task" *after* starting three workers, at which point idle sits at forty-one per cent and the kernel at forty-two, so the sentence would have been spoken over a screen showing the opposite.

**Result.** Both fixed, and the same pass confirmed the parts that did work: ending one worker leaves the other two climbing, the processor hog now runs its full ten seconds, and the preemption demonstration reports forty forced switches in the window against forty-one measured from the shell.

The lesson is the cheapest one in the project and the easiest to skip: a demonstration script is code, and reading it is not testing it.

---
