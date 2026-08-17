# Technical Report — Challenges Faced

**Project:** MyOS, a bare-metal x86 operating system kernel
**Course:** Operating Systems
**Submitted:** *(date)*
**Authors:** *(names)*
**Repository:** *(URL)*

Each challenge below is described in STAR format:

- **Situation** — what the challenge was
- **Task** — what we were doing previously
- **Action** — what we did to address the issue
- **Result** — the outcome of our action

Fourteen technical challenges are recorded, in the order they were encountered. Each is traceable to the commit that resolved it.

A theme runs through most of them and is worth stating first: **on bare metal, "it compiled" tells you almost nothing about whether the program is correct.** There is no operating system, no loader and no runtime beneath this code, so the layout of the binary, the ordering of hardware operations, and whatever the optimiser decided to do with your source are all part of correctness. Four of the fourteen were introduced by a tool doing exactly what it was asked, in a context where nothing existed to catch the consequences.

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

**Result.** The kernel now boots identically with and without a serial port, verified by running it both ways: 22 self-tests pass in each case and the screen output is identical. Dropping debug output when the hardware is missing is survivable; hanging the kernel in order to deliver it is not. We found this by asking what our emulator was being forgiving about, which turned out to be a more productive question than testing the same configuration again.

---

## 15. Toolchain and workflow

**Situation.** We began this project with no command-line experience: no terminal use, no compiling from a shell, no version control beyond a graphical client, and no experience with a debugger. Bare-metal development requires all of these and provides none of the feedback an IDE gives — there is no runtime to report an error, and a mistake often produces silence rather than a message.

**Task.** *(Fill in: how were you working before this project — GitHub Desktop, IDE "Run" buttons, no build system?)*

**Action.** The entire toolchain was placed inside a container image — cross-compiler, assembler, emulator, bootloader tools and debugger — so that nothing had to be installed on the host machine and every team member gets an identical environment from a single command. *(Fill in what you personally did: running builds, reading compiler errors, using git from the command line.)*

**Result.** *(Fill in.)* One finding is worth recording regardless: the single most valuable piece of infrastructure turned out to be logging serial output to a file, because it is the only thing that survives the machine resetting itself. We set it up on the first day rather than after first needing it, which meant that when crashes did occur we already had a record of what happened immediately before.

---

## 16. *(Add any further challenges you encountered)*

**Situation.**

**Task.**

**Action.**

**Result.**
