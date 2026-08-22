# What to showcase, and how

The features that make this an operating system are not things you run. They
are running now, and they have been since the machine booted. The scheduler is
scheduling, the MMU is translating every address, the timer has fired several
thousand times since the prompt appeared. Nobody typed a command to start any
of it.

So the shape of a good demonstration is not *"here is a feature, watch me
invoke it."* It is:

1. **Why does this exist?** — the problem it solves. Without it, what breaks?
2. **How is the OS using it right now?** — the counter on screen that is already
   moving.
3. **What does it look like in code?** — the ten lines that do the work.

The commands in this project are instruments, not features. `preempt off` does
not turn on a feature; it turns the scheduler *off* so you can see what it had
been doing all along. That distinction is worth saying out loud on camera.

---

## Part 0 — The problem statement

### The project

A modern operating system is tens of millions of lines, and the mechanisms that
make it an operating system — scheduling, address translation, privilege — are
buried under decades of abstraction. You can use one for years without ever
seeing them.

**The problem:** demonstrate that those mechanisms are understood, not just
described. Reading about a context switch is not the same as being able to point
at the fifteen instructions that perform one.

**The approach:** build a kernel small enough that every mechanism is visible and
verifiable, on hardware real enough that the mechanisms are the genuine ones —
a real IDT, real page tables, real ring transitions — rather than a simulation
of them. Then make each one demonstrable, so the claim can be checked rather
than taken on trust.

**How it is verified:** 34 in-kernel checks run at every boot. They do not read
back what the kernel printed; each one either turns a mechanism off to prove it
was load-bearing, or reads a value the CPU itself wrote. `make test` boots
headless and fails the build if any of them fail.

### Per mechanism

Every row is a decision, and in each one there was an obvious cheaper answer
that was rejected for a stated reason. This is the table to have open if you are
asked "why did you do it that way."

| The problem | The obvious answer | What TazOS does | What that bought |
|---|---|---|---|
| Share one CPU between tasks | Let tasks yield when they feel like it | The timer interrupt takes the CPU away | One bad loop cannot freeze the machine. `preempt off` shows the difference directly |
| Know when hardware needs attention | Poll every device in a loop | One interrupt vector per device | The CPU can halt when idle. In `top` the `kbd` counter stays still until you type |
| Catch null-pointer bugs | Check pointers in software | Leave page 0 unmapped and let the MMU fault | Free, enforced on every access, and impossible to forget at a call site |
| Stop user code touching hardware | Don't call the dangerous functions | Ring 3, with one `int 0x80` gate at privilege 3 | The CPU enforces it regardless of what the code says |
| Draw a desktop at 1920x1080 | Redraw the screen every frame | Track what changed; save the pixels under the cursor | 8.3 MB per frame became two small rectangles |
| Move a window | Redraw the scene at the new position | Rasterise once, then move pixels and repaint only the uncovered strip | Idle CPU during a drag went from 0% to 33% |
| Get pixels to the display | Copy the box containing everything that changed | Copy the rectangles that changed | A fast pointer flick costs 4 KB instead of a full-width copy |
| Put a photograph in the kernel | Embed a PNG and write a decoder | Decode at build time; store a palette and one index per pixel | No image decoder in the kernel, and 518 KB instead of 1.5 MB |
| Render readable text | A bitmap font, or a TrueType parser in the kernel | Bake anti-aliased coverage atlases at build time | Real glyph shapes for one multiply per pixel, and no parser |
| Make window moves faster still | Hand the blit to the GPU | Tried it, measured it, it was slower, left it off | An evidence-based decision instead of a plausible one |
| Know that any of this works | Run it and see if it looks right | 34 checks that disable mechanisms or read CPU-written values | Regressions fail the build |

### Where it is deliberately not better

Saying this unprompted is worth more than being caught by it.

- **Memory is not isolated between rings.** Instruction privilege is enforced by
  the CPU; the address space is not separated. The whole first 4 MB is marked
  user-accessible. Separating it properly would mean giving user code its own
  linker section on its own pages — a scope decision, made and documented.
- **The scheduler is round-robin only.** No priorities, no aging.
- **It has never run on real hardware.** It is a GRUB rescue ISO and should boot
  on a BIOS or CSM machine, but that has not been tried, and claiming otherwise
  would be exactly the sort of unverified assertion the self-test exists to
  avoid.

---

## Part A — The machinery that never stops

Each of these is running before you touch anything. The third column is the
evidence already on screen, which is what makes the point that it is innate.

### 1. Interrupts — the CPU's only way to hear from hardware ★

**Why it exists.** A processor executes instructions; it has no way to notice
that a key was pressed. Either the kernel polls every device forever, wasting
the whole machine, or the hardware interrupts the CPU and the CPU jumps to a
handler. Everything else in this list depends on that mechanism working.

**Running right now.** `top` — the `irqs timer` counter climbs about a hundred
times a second while you do nothing at all. The `kbd` counter next to it only
moves when you type. Two devices, two independent counters, no polling.

**Code.** [`kernel/isr.asm`](../kernel/isr.asm) — 256 stubs, one per vector,
each pushing a number and jumping to common code.
[`kernel/idt.c`](../kernel/idt.c) — the table that tells the CPU where they are.

> "The table has 256 entries because the processor has 256 vectors. The CPU
> looks up the handler itself — the kernel never checks whether a key was
> pressed."

### 2. The timer, and preemptive scheduling ★

**Why it exists.** One CPU, several tasks. If tasks had to hand the CPU back
voluntarily, one bad loop would freeze the machine — that is what Windows 3.x
and classic Mac OS did, and why one hung program took the system with it. The
timer interrupt takes the CPU away instead.

**Running right now.** `tasks` — the idle task's tick count rises whenever the
kernel has nothing to do, and the context-switch counter is already in the
hundreds of thousands. In Task Manager the tick strip shows idle samples
scrolling past.

**Code.** [`kernel/task.c:321`](../kernel/task.c) — `task_tick`, called from the
timer handler; it is the whole of preemption.

**If you want to prove it rather than assert it:** `preempt off`, `spawn 3`,
`preempt on`. The interleaving disappears and comes back. Frame it as an
instrument: *"I am turning the scheduler off to show you it was doing something."*

### 3. Context switching

**Why it exists.** Switching tasks means the CPU must resume code it left
mid-instruction-stream, with the right registers and the right stack. A task is
really just a stack plus a saved register set.

**Running right now.** Every one of those switches in `tasks` went through this
function.

**Code.** [`kernel/context.asm:23`](../kernel/context.asm) — about fifteen
instructions.

> "Push the callee-saved registers, swap the stack pointer, pop the other
> task's registers back. The `ret` at the end returns into a different task
> than the one that called."

### 4. Virtual memory ★

**Why it exists.** Two reasons here: a null pointer should crash rather than
quietly corrupt whatever is at address zero, and user code must not reach kernel
memory. Both are enforced by the MMU on every single memory access — millions
per second, in hardware.

**Running right now.** `mmu` reports paging on with the mapped size. Then:

```
mmu 0
mmu 100000
```

Address 0 walks to a page-table entry marked not-present — that is *why* a null
dereference faults. Address 0x100000 (1 MB, where the kernel lives) resolves to
a real physical page.

**Code.** [`kernel/paging.c:37`](../kernel/paging.c).

> "The first 4 MB uses 4 KB pages purely so a single page at address zero can be
> left unmapped. Above that, 4 MB pages, which need no second level at all."

### 5. Privilege separation ★

**Why it exists.** A user program must not be able to write to hardware ports or
disable interrupts. Not "should not" — must not, enforced by the processor, so
that a bug or a hostile program cannot do it regardless of what its code says.

**Running right now.** Ring 3 is set up at boot and the `int 0x80` gate is
sitting in the IDT waiting. The self-test exercises it every boot.

**Code.** [`kernel/syscall.c:102`](../kernel/syscall.c) — the hand-built `iret`
frame that drops into ring 3.

> "`iret` does not care that we were never in ring 3 to begin with. Build the
> stack frame it expects and it will return into user mode."

**To show it:** `user` — the task tries a privileged instruction, the CPU
refuses, the kernel kills that task and keeps running. Then `user --syscall`,
which goes through the one gate with a privilege level of 3.

### 6. The heap

**Why it exists.** There is no `malloc` here — nothing underneath provides one.
Every window, every file's contents, every task stack comes out of this
allocator. If it leaks or corrupts a header, the desktop dies.

**Running right now.** Task Manager's Memory row, and `meminfo`. The number
moves as windows open and files are written.

**Code.** [`kernel/heap.c:49`](../kernel/heap.c) — first fit, splitting,
coalescing, magic headers to detect corruption.

### 7. Device drivers

**Why they exist.** Nothing about a PCI device's address is known in advance;
the firmware assigns it at boot. The kernel has to discover the hardware and
then speak each device's protocol.

**Running right now.** The clock in the taskbar corner is the CMOS RTC, read
every second. The pointer is the PS/2 mouse on IRQ 12. `lspci` lists what was
found on the bus; `disk` shows the IDE drive and the sectors written to it.

**Code.** [`kernel/pci.c`](../kernel/pci.c), [`kernel/mouse.c`](../kernel/mouse.c),
[`kernel/ata.c`](../kernel/ata.c), [`kernel/rtc.c`](../kernel/rtc.c).

---

## Part B — Engineering stories

These are the strongest material in the project, and the part nobody else will
have. Each is a real defect with a diagnosis and a measured result. The
structure is always: **it was broken → here is why → here is the fix → here is
the number.**

All of them are written up in [`CHALLENGES.md`](../CHALLENGES.md) in STAR
format, so the video and the report agree.

### The mouse used to lag, and now it does not ★★

**Symptom.** The pointer trailed several centimetres behind the mouse.

**Diagnosis.** Every frame began by filling the whole screen and redrawing
everything. At 1920x1080 and four bytes per pixel that is 8.3 MB per frame —
roughly 1.6 GB/s of memory traffic to move a pointer a few pixels. The machine
was not slow; it was being asked to do an enormous amount of pointless work.

**Fix.** Two changes. The scene is only rebuilt when something in it actually
changed. And the cursor saves the pixels underneath it before drawing, so
erasing it costs one small rectangle instead of a whole frame.

**Code.** `cursor_erase` / `cursor_draw` in [`kernel/gui.c`](../kernel/gui.c).

> "The expensive operation was never drawing. It was drawing things that had
> not changed."

### Dragging a window pinned the CPU at 100% ★★

**Symptom.** Dragging any window took the whole processor.

**Diagnosis, with numbers.** Over a ten-second drag the idle task received
**zero** ticks and the context-switch counter did not move at all — the render
loop had the CPU to itself. Every mouse packet was paying for a full rebuild:
the entire wallpaper, a re-render of every window including all of its
anti-aliased text, and an 8 MB present.

**Fix.** None of that work was needed, because a dragged window's *contents* do
not change — only its position. The window is now rasterised once when the drag
starts; each step restores the strip it uncovered and blits the cached pixels at
the new position. That is what a compositor does.

**Result.** Idle went from 0% to 33%.

**Code.** `draw_drag_step` in [`kernel/gui.c:4056`](../kernel/gui.c).

### The pointer blinked, and windows tore

**Diagnosis.** The present function copies the *bounding box* of everything that
changed. Moving the pointer marks two small rectangles — where it was and where
it is — and the box containing them grows with how fast you move, so a quick
flick turned a 4 KB update into a full-width copy. Video memory is uncached and,
under a hypervisor, every write is intercepted, so a copy that large does not
finish inside one screen refresh and you see it half-applied.

**Fix.** Push the rectangles that changed, not the box that contains them.

> "The pointer now costs the same 4 KB however fast you move it."

### A PS/2 mouse has no scroll wheel ★

**Why this one is interesting.** It cannot be worked out from first principles.
The three-byte PS/2 packet has no wheel field and there is no command that asks
for one. The extension is unlocked by a *knock*: set the sample rate to 200,
then 100, then 80, in exactly that order. A mouse that recognises the sequence
starts reporting device ID 3 and sending a fourth byte. One that does not stays
in the three-byte protocol, which is what makes it safe to try blindly.

**Code.** [`kernel/mouse.c`](../kernel/mouse.c) — the `knock` array.

> "No amount of reasoning about the packet produces the idea of setting the
> sample rate three times. That only comes from the documentation."

### The kernel hung, and the log told me why

**Symptom.** `demo` scrolled forever in the console and froze the desktop.

**Diagnosis.** The serial log was 92 KB, of which 87 KB was a single line: one
monitor row followed by eighty-seven thousand spaces. The monitor pads each row
to a fixed width and asked the *VGA driver* for the cursor column — correct
until the framebuffer console arrived, after which that cursor never moved
again and the padding loop could never reach its limit.

**Why it is worth telling.** The original code was already a fix for an earlier
bug: measuring the cursor instead of counting by hand. It was the right
instinct, and it still broke, because it hard-wired an assumption about which
device was receiving output — and that assumption expired silently when a new
one was added.

### The GPU measurement that came out negative ★★

**What I did.** Moving a window is a block copy, which is exactly what a 2D
engine is for, and the adapter has a `RECT_COPY` command. So I wired the drag
path to use it.

**What happened.** It got slower. Repeatably: idle 33% with it off, 27–30% with
it on.

**Why.** QEMU executes that command on the host CPU, so there is no blit engine
being saved; and the FIFO is asynchronous, so before the CPU may write the
uncovered strips it has to wait for the adapter to finish reading — and that
wait polls a register over an I/O port, which is a virtual-machine exit per
read, every frame.

**What I did about it.** Left the code in, defaulted it off, and put the
measurement in the comment.

> "I had a good reason to expect it to be faster. It was not, so it is off. The
> flag is one line if the numbers come out differently on real hardware."

This is the most professional thing in the project. A measured negative result,
kept and documented, is worth more than an optimisation that was never checked.

---

## Part C — Three minutes

Boot before recording; the self-test is a third of the budget. Start on the
prompt.

**0:00 — The problem** *(20 s)*
Have `top` running behind you.
> "An operating system is tens of millions of lines, and the parts that make it
> one — scheduling, address translation, privilege — are buried so deep you can
> use a computer for years without seeing them. The problem I set myself was to
> show I understand those mechanisms rather than describe them. So this is a
> kernel small enough that every one of them is visible, on hardware real enough
> that they are the genuine mechanisms."

> "Nothing is happening on this machine right now and the timer has already
> fired four thousand times. That counter is the operating system running.
> Everything I am about to show you was working before I typed anything."

**0:15 — Interrupts and the scheduler** *(45 s)* ★
Point at the climbing `irqs timer` and `switches` in `top`. Type something and
show `kbd` move independently. Cut to `context.asm:23`.
> "One CPU, several tasks. If tasks had to give the CPU back voluntarily, one
> bad loop would freeze the machine. The timer takes it away instead — and this
> is the switch: save the registers, swap the stack pointer, and the `ret` at
> the end returns into a different task."

Optional, if the pace allows: `preempt off` / `on` as proof.

**1:00 — Memory protection** *(35 s)* ★
```
mmu 0
mmu 100000
```
> "Address zero is deliberately not mapped, which is why a null pointer faults
> instead of corrupting memory. A megabyte up, where the kernel lives, resolves
> to a real page. The MMU checks this on every access, in hardware."

Then `user`:
> "And a ring 3 task that tries a privileged instruction is killed by the
> processor. Not by a check in my code."

**1:35 — Why the desktop is smooth** *(60 s)* ★★
`gui`, drag a window.
> "This used to lag badly and dragging took 100% of the CPU. Every mouse packet
> redrew the whole desktop — eight megabytes a frame to move a rectangle."

Cut to `draw_drag_step`.
> "A dragged window's contents do not change; only where they are does. So it is
> rasterised once and each frame moves those pixels and repaints only the strip
> it uncovered. That is what a compositor does. Idle CPU went from zero to a
> third."

Then Notepad → save → File Explorer.
> "The file manager keeps no copy of the filesystem. It walks it every frame,
> which is why the file I just saved is already there."

**2:35 — The negative result** *(15 s)* ★
> "I also tried handing window moves to the graphics adapter's blit engine. It
> measured slower, so it is off — the code is there with the numbers in the
> comment."

**2:50 — Close** *(10 s)*
> "No operating system underneath any of it — it boots from GRUB, sets up its
> own descriptor tables, drives the hardware directly and draws its own desktop.
> Memory is not isolated between rings and it has never run on real hardware.
> Both were scope decisions, and both are written down."

---

## Reading a file on camera

Have the files open in tabs beforehand and jump to the line — do not scroll
looking for it. Three that reward being shown, in order:

1. [`kernel/context.asm:23`](../kernel/context.asm) — fifteen instructions, and
   it is the whole idea of multitasking
2. [`kernel/syscall.c:102`](../kernel/syscall.c) — the `iret` into ring 3
3. `draw_drag_step` in [`kernel/gui.c:4056`](../kernel/gui.c) — the compositor

## Do not claim

- **Real hardware** — never attempted.
- **3D acceleration** — the driver is complete and the adapter advertises the
  capability, but reports a 3D hardware version of zero, so the pipeline is not
  available to this guest and the driver declines. That it declines rather than
  issuing commands into a FIFO that will not execute them is the good answer.
- **Memory isolation between rings** — instruction privilege is enforced;
  the address space is not isolated. A documented scope decision.
