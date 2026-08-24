# Technical Report — Challenges Faced

**Project:** TazOS, a bare-metal x86 operating system kernel
**Course:** Operating Systems
**Submitted:** *(date)*
**Authors:** *(names)*
**Repository:** *(URL)*

Each challenge below is described in STAR format:

- **Situation** — what the challenge was
- **Task** — what we were doing previously
- **Action** — what we did to address the issue
- **Result** — the outcome of our action

Thirty technical challenges are recorded, in the order they were encountered. Each is traceable to the commit that resolved it.

A theme runs through most of them and is worth stating first: **on bare metal, "it compiled" tells you almost nothing about whether the program is correct.** There is no operating system, no loader and no runtime beneath this code, so the layout of the binary, the ordering of hardware operations, and whatever the optimiser decided to do with your source are all part of correctness. Four of them were introduced by a tool doing exactly what it was asked, in a context where nothing existed to catch the consequences, and two more by a register or a protocol meaning something other than what the documentation to hand said it meant. Two of the last six are measurements rather than defects: one optimisation that turned out to be slower and was kept switched off with the numbers beside it, and one accounting error that made idle threads look like busy ones.

---

## 1. GRUB refused to load the kernel

**Situation.** The kernel compiled and linked without a single warning, but the GRUB bootloader rejected it with `no multiboot header found`. The header was plainly present in our source and the magic number was correct, so the error told us nothing about what was actually wrong.

**Task.** We had written the multiboot header as an assembly section and assumed that anything defined in the source would appear near the beginning of the output file. Our first instinct was that the constants were wrong, so we spent time re-checking the magic value and the checksum arithmetic against the Multiboot specification. All of it was correct.

**Action.** We stopped reading the source and inspected the compiled binary instead. `objdump -h` showed the code section sitting at file offset `0x2000`, and searching the raw bytes located the magic value at **byte 8192** — while GRUB only searches the first 8192 bytes, meaning offsets 0 to 8191. The header was one byte outside the window. The cause was a section we had never written: the linker was automatically inserting a build-identification note at the very start, consuming the first page and pushing everything else onto the next. We disabled it with `--build-id=none` and added an explicit discard rule to the linker script.

**Result.** GRUB loaded the kernel immediately, with the header now at offset 4096. We added an automatic `grub-file --is-x86-multiboot` check to the build so this failure can never again reach the point of being debugged by hand. More usefully, it taught us to inspect the produced binary before re-reading the source, which shortened several later problems from hours to minutes.

---

## 2. A symbol that was both missing and duplicated

**Situation.** After adding interrupt handling, the link failed with `undefined reference to isr_handler` — for a function that visibly existed and had compiled cleanly. At the same time it reported `multiple definition of irq15`, naming the same file as both the duplicate and the original.

**Task.** Our Makefile generated object filenames by pattern substitution, turning `kernel/anything.c` and `kernel/anything.asm` into `build/anything.o`.

**Action.** A symbol reported as simultaneously absent and duplicated is not a code problem, so we looked at the build instead. We had two source files named `isr.c` and `isr.asm`, and both were producing `build/isr.o`. Whichever compiled second silently overwrote the first, so the C symbols and the assembly symbols could never exist in the same file at the same time. We changed assembly objects to use a distinct `.asm.o` suffix.

**Result.** The link succeeded. The change also prevented the identical collision from recurring later, when we added a `context.c` and `context.asm` pair for the scheduler — where the same bug would have appeared in a far more confusing form, because by then the kernel would have been running.

---

## 3. An interrupt acknowledgement that would never have executed

**Situation.** This one we caught by reasoning rather than by debugging. While writing the scheduler it became clear that the hardware timer would stop firing permanently the first time we switched to a newly created task.

**Task.** Our interrupt dispatcher followed the conventional shape found in most examples: run the registered handler, then send the end-of-interrupt signal to the interrupt controller.

**Action.** The timer handler is what drives the scheduler, and a context switch does not return — it swaps stacks and resumes a completely different task. If that task was newly created it has never been inside the dispatcher, so an acknowledgement placed *after* the handler would simply never run. The controller would go on believing the timer interrupt was still being serviced and would deliver no further ones. We moved the acknowledgement to before the handler call, which is safe because the CPU disables interrupts on entry, so nothing can arrive in between.

**Result.** Preemption worked on the first attempt. It is worth recording precisely because it was avoided rather than suffered: the symptom would have been a total freeze with no output and no fault, which is among the hardest possible things to diagnose backwards.

---

## 4. Format specifiers corrupting every value after them

**Situation.** Our per-task CPU-time table printed as `%-10s id=1089572 state=%-8s ticks=1`, with the format codes appearing literally on screen and the task ID numbers obviously wrong.

**Task.** We had written our own `printf` (there is no C library available here) supporting only a plain conversion character and an optional zero-padded width, which was all the boot messages had needed.

**Action.** The visible symptom was that `%-10s` was unsupported and printed as text. The serious problem was invisible: our parser consumed the wrong number of variable arguments, so every value after the unsupported specifier was read from the wrong position on the stack. The nonsensical task IDs were actually the memory addresses of the task *names*, printed as integers. We rewrote the formatter to render each field into a buffer first and then apply width, alignment and padding separately.

**Result.** Tables render correctly, which the live kernel monitor depends on entirely. The lesson was that the cosmetic symptom and the dangerous one were different: literal `%-10s` merely looked wrong, while the argument misalignment was producing confident, plausible-looking numbers that were pure nonsense.

---

## 5. A race condition demonstration that produced the correct answer

**Situation.** We built a demonstration in which two tasks increment a shared counter without any locking, expecting updates to be lost. Every run returned exactly the right total, so the demonstration proved nothing at all.

**Task.** Each task performed a read, then a short fixed-length delay loop, then a write, repeated several thousand times.

**Action.** The gap between reading and writing was far too small relative to the scheduler's time slice. At 50 milliseconds per slice and a critical section lasting a microsecond or two, the chance of the switch landing between the read and the write was roughly one in twenty-five thousand per iteration. We also realised a fixed delay count is the wrong tool regardless, because how long it takes depends entirely on the speed of the machine running it. Instead we made the kernel measure, at startup, how many loop iterations fit inside one timer tick, and then hold each update open for a fraction of that measured value. We also shortened the time slice.

**Result.** Updates were reliably lost. The measurement approach means the demonstration behaves the same way on a fast machine and a slow one, which a hardcoded delay could never guarantee.

---

## 6. The same demonstration then became too reliable

**Situation.** Having made the race condition appear, the result pinned to exactly half the expected total on every single run. A separate check we had written — that results should differ between runs — began failing roughly one boot in three.

**Task.** Each iteration now waited for the timer tick counter to change before writing its value back.

**Action.** Waiting on the clock meant both tasks were waiting on the *same* clock edge. They fell into perfect lockstep, lost an update on every iteration without exception, and produced a number reproducible to the digit. That still technically demonstrates a race, but a result identical on every run is indistinguishable from a hardcoded one, which defeats the purpose of showing it. We narrowed the window to a randomised span of up to a quarter of a tick. The quarter matters: our measurement was taken with one task running alone, but two tasks share the processor, so the same work occupies roughly twice the elapsed time.

**Result.** Totals now scatter genuinely — 93, 78, 84, 63, 89 across five consecutive runs — while the locked version is exact every time. Verified stable across four separate boots. A demonstration can fail by being too deterministic just as easily as by not triggering at all.

---

## 7. A binary that contradicted its own source code

**Situation.** After changing a constant in a header file from 8000 to 100, part of the system used the new value while another part continued printing 8000. The source and the running program disagreed, with no warning from any tool.

**Task.** Our Makefile declared that each object file depended only on its own `.c` file.

**Action.** The file printing the stale value had not itself been edited, so the build system saw no reason to recompile it, and it remained linked against the old value of a constant defined in a header. We enabled automatic dependency generation, which makes the compiler emit a list of every header each file included, and told the build to use those lists.

**Result.** Editing a header now correctly rebuilds everything that depends on it. This is ordinary practice in C projects, but its absence was unusually disorienting here, because the failure presents as code that appears to be ignoring its own source.

---

## 8. The compiler deleted a deliberate crash

**Situation.** We wrote a command to divide by zero on purpose, to demonstrate that the kernel catches processor exceptions. It reported `still alive — the fault did not fire`. No exception occurred and the task ran to completion.

**Task.** The division was written in C, using a variable holding zero that we had marked `volatile` specifically so the compiler could not simplify it away.

**Action.** We disassembled the compiled function and found **no division instruction anywhere in it**. The compiler had replaced the whole expression with a comparison and a conditional move. Its reasoning is sound: dividing by zero is undefined behaviour in C, so a compiler is entitled to assume it never happens, and `1 divided by x` is non-zero only when `x` is 1 or −1. Our `volatile` marking had forced the *loading* of the variable, but not the division that used it. We wrote the instruction directly in assembly, where the compiler has nothing left to reason about.

**Result.** All the fault demonstrations now raise genuine processor exceptions, and each is contained — the offending task is killed while the kernel and shell continue running. The wider lesson is sharper than the fix: **you cannot reliably trigger a processor exception from C**, because every method of doing so is undefined behaviour, and undefined behaviour is precisely what an optimiser is permitted to assume away. We applied the same reasoning pre-emptively when adding the null-pointer demonstration later.

---

## 9. User mode crashed on its very first instruction

**Situation.** Our first attempt at running code at the unprivileged level succeeded in switching privilege — the processor confirmed it — but the very first instruction fetched afterwards raised a memory fault, with an error code meaning "the page exists, but you are not allowed to touch it".

**Task.** Our paging code set the user-accessible flag on every individual page covering the memory where the user code and its stack live.

**Action.** The error code was the clue. "Present" meant the address was mapped correctly, so the problem was permission, not location. Memory permissions on x86 are checked at *every* level of the page-table structure and combined with a logical AND. We had set the user flag on the individual pages but not on the higher-level entry pointing at that group of pages, and a user-accessible page reached through a kernel-only pointer is still kernel-only. Adding the flag at both levels fixed it.

**Result.** Unprivileged code runs correctly. We also recorded a consequence honestly rather than quietly: this fix makes a whole 4 MB region user-accessible, so memory is *not* isolated between the kernel and user tasks in our implementation. What we do genuinely enforce is instruction privilege — unprivileged code cannot touch hardware — and our documentation states the distinction rather than implying more than we built.

---

## 10. Every system call reported as an unknown crash

**Situation.** With unprivileged mode working, our system call instruction reached the kernel but was reported as `CPU EXCEPTION 128: unknown`, and the calling task was killed as though it had crashed.

**Task.** Our exception dispatcher looked up a registered handler before falling through to its error-reporting path.

**Action.** The lookup was written to check `if (number < 32 && handler exists)`, dating from when the only registered handlers were processor exceptions, which occupy numbers 0 to 31. Our system call uses number 128, so the condition excluded it and the correctly-registered handler was never consulted. Removing the upper bound fixed it.

**Result.** System calls dispatch correctly. The bug itself is trivial; what makes it worth recording is that it was introduced by an assumption that was *true when written* and silently became false. The condition encoded "handlers are only ever exceptions", which stopped being true the moment we added a system call.

---

## 11. The live monitor scrolled instead of refreshing

**Situation.** Our live kernel monitor was meant to redraw in place, like the `top` command on Linux. On screen it printed the characters `[H` and scrolled, stacking each successive frame down the display.

**Task.** The monitor sent an ANSI escape sequence through our print function to move the cursor to the top-left corner between frames.

**Action.** ANSI escape sequences are a *terminal* convention. Our serial output is read by a terminal and honours them, but our screen output writes directly into video memory as a grid of character cells, which has no concept of escape sequences and therefore rendered the bytes literally. We added a function that moves the hardware cursor directly on the screen side and emits the escape sequence only on the serial side.

**Result.** The display refreshes in place correctly on both. The underlying mistake was treating two genuinely different devices as interchangeable because they happened to share a programming interface.

---

## 12. A demonstration that overstated its own evidence

**Situation.** Our race-condition command printed the message "totals differ from each other and from the expected value" whenever any run was incorrect — including runs where all three totals were identical, which a screenshot captured plainly.

**Task.** The summary chose its message based on a single flag recording whether every run had produced the expected total.

**Action.** The message was asserting something the code had never checked. We added a second flag that tracks whether the results genuinely differed from one another, and split the summary into three honest cases: nothing lost, updates lost with varying totals, and updates lost with identical totals.

**Result.** The command now reports what actually happened rather than what usually happens. The failure mode is worth naming: our own output was arguing for the correctness of the system using evidence that was not on the screen. That is exactly the kind of claim a reader should distrust, and it is why our verification approach elsewhere relies on turning features off and on hardware-reported values rather than on the kernel describing itself.

---

## 13. A synchronisation demonstration revealed unsynchronised output

**Situation.** We added a producer/consumer demonstration that prints a marker for each item. Its output came out as `c14 cP16 P17 P18 15` — one task's marker had been split down the middle, with three of another task's markers wedged inside it.

**Task.** Our print function wrote straight to the screen and the serial port with nothing coordinating access. Every other shared structure in the kernel — the memory allocator's block list, the task list, the shared buffer itself — was already protected, but the console had never been thought of as shared at all.

**Action.** The splitting was correct behaviour for unprotected code: a task interrupted partway through printing resumes later, and whatever ran in between wrote to the same screen. The obvious fix is a lock around the print function, and it is the wrong one — our crash handler prints, and a crash can occur *inside* a print call, at which point the handler would wait forever for a lock held by the very task it interrupted. We added a counter to the scheduler that temporarily defers task switching, and wrapped the print function in it. Interrupts stay enabled, so the timer keeps running and CPU time is still charged correctly; only the switch is postponed. A counter nests safely where a lock would deadlock.

**Result.** Output is now atomic per call, and the demonstration reads cleanly. Two things make this worth recording. It was found by *writing a feature*, not by testing — the new demonstration exposed a defect in code that had been running since the first day. And it is a lesson in what counts as shared state: we had been thinking of the console as an output *service* rather than as a resource two tasks could contend for, and that framing is exactly what hid it.

---

## 14. A polling loop that could hang the kernel on real hardware

**Situation.** We wanted to run the finished kernel in VMware and possibly from a USB stick, rather than only in the emulator we had developed against. Reviewing the code for assumptions that hold in an emulator but not elsewhere, we found that our serial port driver waits in an unbounded loop for the hardware to report itself ready to send.

**Task.** The driver assumed a serial port was present, because our emulator always provides one. Every message the kernel prints passes through this function.

**Action.** VMware does not give a virtual machine a serial port unless one is explicitly configured, and most modern PCs have no physical serial port at all. Reading a port that does not exist returns whatever the bus happens to float to — commonly all ones, which coincidentally looks like "ready", but zero on some hardware, which looks permanently busy. In that case the loop never exits and the kernel hangs inside its very first print, before anything reaches the screen: a black display with no diagnostic whatsoever, which is the hardest failure imaginable to work backwards from. We added a startup probe that puts the port into loopback mode, writes a byte and checks whether the same byte comes back — a real chip returns it, an absent one does not — and made the transmit loop give up after a bounded number of attempts.

**Result.** The kernel now boots identically with and without a serial port, verified by running it both ways: 30 self-tests pass in each case and the screen output is identical. Dropping debug output when the hardware is missing is survivable; hanging the kernel in order to deliver it is not. We found this by asking what our emulator was being forgiving about, which turned out to be a more productive question than testing the same configuration again.

---

## 15. Toolchain and workflow

**Situation.** We began this project with no command-line experience: no terminal use, no compiling from a shell, no version control beyond a graphical client, and no experience with a debugger. Bare-metal development requires all of these and provides none of the feedback an IDE gives — there is no runtime to report an error, and a mistake often produces silence rather than a message.

**Task.** *(Fill in: how were you working before this project — GitHub Desktop, IDE "Run" buttons, no build system?)*

**Action.** The entire toolchain was placed inside a container image — cross-compiler, assembler, emulator, bootloader tools and debugger — so that nothing had to be installed on the host machine and every team member gets an identical environment from a single command. *(Fill in what you personally did: running builds, reading compiler errors, using git from the command line.)*

**Result.** *(Fill in.)* One finding is worth recording regardless: the single most valuable piece of infrastructure turned out to be logging serial output to a file, because it is the only thing that survives the machine resetting itself. We set it up on the first day rather than after first needing it, which meant that when crashes did occur we already had a record of what happened immediately before.

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

## 31. *(Add any further challenges you encountered)*

**Situation.**

**Task.**

**Action.**

**Result.**
