**Situation** — GRUB rejected the kernel with `no multiboot header found`, although the header was present in the source and its magic value and checksum were correct.

**Task** — We had written the multiboot header as an assembly section and assumed that a section defined early in the source would appear early in the output binary. Debugging had been limited to re-reading that source against the specification.

**Action** — We inspected the produced binary instead of the source. `objdump -h` put `.text` at file offset 8192, one byte outside the 8192-byte window GRUB scans. The linker was placing a `.note.gnu.build-id` section first and pushing everything back a page. We added `-Wl,--build-id=none` and an explicit `/DISCARD/` block to the linker script.

**Result** — The kernel booted on the next attempt with `.text` at offset 4096. We added `grub-file --is-x86-multiboot` to the build so the failure can never reach a debugging session again, and adopted inspecting the binary as the first debugging step rather than the last.

---

**Situation** — Timer interrupts fired exactly once. The first tick arrived, the scheduler ran, and no interrupt was ever delivered again.

**Task** — The IRQ dispatcher followed the conventional shape: call the registered handler, then send the end-of-interrupt byte to the interrupt controller.

**Action** — That ordering is correct for every handler that returns. The timer handler ends in a context switch and never returns, so the acknowledgement was unreachable and the controller kept waiting for it. We moved the end-of-interrupt byte to before the handler call.

**Result** — Timer interrupts arrive continuously and preemption works. The general lesson was that "clean up afterwards" assumes the code reaches afterwards, which is not true of anything that switches stacks.

---

**Situation** — The first ring 3 task triggered a page fault on its very first instruction, before executing any of its own code.

**Task** — We had marked the pages containing the user program as user-accessible in the page table and expected that to be sufficient.

**Action** — Reading the manual again rather than the code: the processor checks the user bit at *every* level of the translation. The page table entry allowed user access, the page directory entry above it did not, and permission is the AND of the two. We set the user bit on the directory entry as well.

**Result** — The ring 3 task runs. This one could not have been found by inspecting our own code, because our code was doing exactly what we intended — the mistake was in what we believed the hardware did.

---

**Situation** — Dragging any window took the whole processor. Over a ten-second drag the idle task received zero timer ticks and the context-switch counter did not move at all.

**Task** — A drag set the flag meaning "the scene changed", so every mouse packet paid for a full rebuild: the entire wallpaper, a re-render of every window including all its anti-aliased text, and an eight-megabyte copy to the screen.

**Action** — None of that work was needed, because a window's contents do not change while it is dragged — only its position does. The window is now rasterised once when the drag begins, and each step restores the strip it uncovered and blits the cached pixels at the new position.

**Result** — Idle went from zero per cent of the ticks to thirty-three, measured identically before and after. The scheduler runs during a drag again, so input and background work are no longer starved.

---

**Situation** — Moving a window is a block copy, which is what a 2D graphics engine exists for. The adapter advertised the capability and the driver had the command implemented, but nothing had ever called it.

**Task** — All window movement was done by the processor, copying pixels into video memory by hand.

**Action** — We routed the drag through the adapter's block-copy command, then measured it against the same build with the feature disabled, on the same adapter, over the same ten-second drag.

**Result** — It was slower, repeatably: thirty-three per cent idle without it, twenty-seven to thirty with it. The emulator performs the copy on the host processor, so no work is offloaded, and the ordering it forces costs a virtual-machine exit every frame. The code was kept, switched off by default, with the measurement recorded beside the flag.

---

**Situation** — Three background worker threads appeared to consume the entire machine, while the idle task showed almost no time at all.

**Task** — Each worker incremented a counter, yielded, and executed `hlt` to wait for the next timer interrupt.

**Action** — The measurement was wrong before the code was. A halted task is still the current task, so the timer interrupt charged each worker for time it spent doing nothing. We replaced the halt with a real sleep, which marks the task blocked and reschedules, and gave the workers genuine arithmetic to perform so the time they are charged is time they earned.

**Result** — Three workers now take about thirteen ticks a second between them, and the idle task holds around forty per cent with all three running. The reported figures now mean what they say.

---

**Situation** — The kernel implemented system calls correctly — one interrupt gate at privilege level 3, with a ring 3 program the processor kills when it reaches for a hardware port — but the trap instruction appeared on only two lines in the entire source tree, both inside demonstration code.

**Task** — Every application on the desktop is compiled into the kernel and runs at ring 0. Saving a file from the editor called the filesystem directly, as an ordinary function call.

**Action** — Rather than describe an interface nothing used, we routed one real operation through it. Saving a file now places the call number and arguments in registers and executes `int 0x80`; the kernel's dispatcher performs the write. We also added a counter that only increments inside the interrupt handler, so the trap can be demonstrated rather than asserted.

**Result** — Saving from the editor genuinely enters the kernel through the system call gate. Verified with the desktop running and a disk attached: seven calls serviced before the save, eight immediately after, and the file present on disk at the correct size. The scope is stated rather than blurred — one service is routed this way, not the whole desktop.
