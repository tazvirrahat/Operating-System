# TazOS — three-minute walkthrough

Everything in a **> quote block** is meant to be said out loud, word for word.
Everything else is what you do with the mouse.

---

## Before you record

Build with `dev`. Boot `vmware/MyOS.vmx`, wait for `>`, type `gui`. Then open
**Task Manager** (drag it right, it stays open throughout), **Notepad** (click
the white page once), and **Kernel Lab**. Move them apart so nothing overlaps.

The 30–50 second self-test at boot does not go in the take. Start recording once
the desktop is set up.

Have these open in tabs, at the line, ready to cut to:

- `kernel/task.c:348` — `task_tick`
- `kernel/context.asm:23` — `context_switch`
- `kernel/gui.c:4336` — `draw_drag_step`

---

## Intro · 20s

Nothing to click. Just the desktop on screen.

> "This is TazOS — an operating system I wrote from scratch for x86."
>
> "There's nothing underneath it. No Linux, no Windows. It boots from GRUB, sets
> up its own interrupt tables, manages its own memory, and draws this desktop
> itself."
>
> "About fifteen thousand lines, and everything I'm about to show you is running
> live."

---

## Part 1 — Threads · 35s

**Do:** point at **System idle**, then Kernel Lab → **Background workers** →
**Start 3 workers**.

> "This is Task Manager, reading my scheduler live."
>
> "Right now the busiest thing here is the idle task. No work to do, so the
> processor is halted."
>
> "Now I'll start three background threads."

**Do:** point at the three `worker_` rows and the loops counter.

> "There they are. The tick counts are climbing, and they're all different
> numbers. That's real CPU time being split between them."

**Do:** cut to `kernel/task.c:348`.

> "And that's preemption. It runs off the timer interrupt — every tick it
> charges whoever was running, and decides whether to switch."

Leave the workers running.

---

## Part 2 — Preemption · 45s

**Do:** Kernel Lab → **Hog vs Notepad** → **Run with sharing OFF**. Try to type.

> "Here's a program that grabs the CPU and never gives it back. First with
> sharing off."
>
> "Nothing. The mouse is gone too. One bad program has taken the whole machine."

**Do:** wait about three seconds for it to release.

> "And it comes back on its own."

**Do:** click Notepad's page. **Run with sharing ON**. Click the page and type.

> "Same program, sharing on."
>
> "I can type. And it's still running — you can see it there, still burning CPU.
> But the timer takes the processor off it a hundred times a second."
>
> "That's the difference between a machine one bad program can freeze, and one
> it can't."

**Do:** cut to `kernel/context.asm:23`.

> "That's the context switch. Fifteen instructions — save the registers, swap
> the stack pointer, and that `ret` lands in a different thread."

---

## Part 3 — What I optimised · 40s

**Do:** drag a window around for a few seconds.

> "Dragging a window used to take the entire processor. I measured it — over a
> ten-second drag, the idle task got zero ticks."
>
> "Every mouse packet was redrawing the whole desktop. Eight megabytes a frame,
> to move one rectangle."
>
> "But the contents don't change while you drag it. Only the position does. So
> now it's drawn once, and each frame just moves those pixels and repaints the
> strip it uncovered."

**Do:** point at **System idle**, still getting CPU while you drag.

> "I measured it again after — a third of the CPU came back. You can see idle
> still getting scheduled while I drag."

**Do:** cut to `kernel/gui.c:4336`.

> "That's the function."

---

## Part 4 — Locking, and the close · 30s

**Do:** Kernel Lab → **Two programs, one file (unlocked)** → **Write unlocked**,
then open `till.log` in Notepad.

> "Last thing. Two programs writing the same file at once. First without a
> lock."
>
> "The letters tear into each other — A and B interleaved, mid-line."

**Do:** **Two programs, one file (locked)** → **Write with lock**, then open
`till.log` again.

> "Now with a lock. Every line comes out whole. And that's the real file — I'm
> just opening it in the editor."

**The close:**

> "I won't tell you this beats Linux. It doesn't."
>
> "What it is, is fifteen thousand lines you can actually sit down and read, and
> it checks thirty-four of its own guarantees every time it boots."
>
> "Memory isn't isolated between privilege levels, and it's never run on real
> hardware. Both were deliberate, and both are written up."

---

## Timing

429 spoken words. At 150 words a minute that's 2:52 of talking; at a brisker
160 it's 2:41. Add roughly fifteen seconds of silence while you drag, wait for
the hog to let go, and open files.

So: comfortably under three minutes if you talk at a normal clip, and just over
if you're slow. That is tighter than it looks on paper.

If you overrun, drop the `gui.c` cut in part 3 and finish part 4 on "every line
comes out whole."

Read it aloud once with a stopwatch before the take. If you naturally talk fast,
you will have room to spare; if you talk slowly, cut the second sentence of the
close.

---

## Four things that will trip you up

- **Taskbar buttons move.** Focusing a window reorders them. Read the labels,
  don't click by position.
- **Notepad ignores keys until you click its page.** Every time you come back to
  it. This is the most likely reason you'll need a retake.
- **Sharing OFF kills the mouse too**, for about three seconds. That's the demo
  working. Don't click around.
- **The unlocked race is a race.** Now and then it comes out clean. Run it again.

If Notepad's Save ever says "RAM only", the disk didn't attach — don't claim the
file survives a reboot.

---

## Say "I measured"

Everything you quote is on screen live except the two drag figures — zero idle
ticks before, a third of the CPU back after. Those came from a measured run, so
say so. It's the phrase that separates a number from a guess.
