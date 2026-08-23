# TazOS — three-minute walkthrough

## Before you record

Build with `dev`. Boot `vmware/MyOS.vmx`, wait for `>`, type `gui`. Then open
**Task Manager** (drag it right, it stays open throughout), **Notepad** (click
the white page once), and **Kernel Lab**. Move them apart so nothing overlaps.

The 30–50 second self-test at boot does not go in the take. Start recording once
the desktop is set up.

Have these open in tabs, at the line, ready to cut to:

- `kernel/context.asm:23`
- `kernel/task.c:348`
- `kernel/gui.c:4322`
- `kernel/sync.c:40`

---

## Beat 1 — Threads · 40s

**Do**

1. Point at **System idle** in Task Manager, Ticks climbing.
2. Kernel Lab → **Background workers** → **Start 3 workers**.
3. Point at the three `worker_` rows — different Ticks on each.

**Say**

> "That's Task Manager reading my scheduler, live. Right now the busiest thing
> here is the idle task — no work, so the processor is halted."
>
> "Three threads. They show up straight away and each one's tick count climbs.
> Different numbers, so time is genuinely being divided between them."

**Code** — `kernel/task.c:348`, `task_tick`.

> "That's it. It runs off the timer interrupt and it's the whole of preemption."

Leave the workers running.

---

## Beat 2 — Preemption · 50s

**Do**

1. Kernel Lab → **Hog vs Notepad** → **Run with sharing OFF**. Try to type in
   Notepad. Nothing. Wait about three seconds.
2. Click Notepad's page. **Run with sharing ON**. Click the page again. Type.
3. Point at `hog` in Task Manager, ticks climbing.

**Say**

> "Now a program that never gives the CPU back. Sharing off, and it's taken the
> whole machine — mouse is gone, Notepad won't take a key."
>
> "Same program, sharing on. Still running, still burning CPU, you can see it
> there. But I can type. The timer takes the processor off it a hundred times a
> second."
>
> "That's the difference between a machine one bad program can hang and one it
> can't."

**Code** — `kernel/context.asm:23`.

> "Fifteen instructions. Save the registers, swap the stack pointer, and the
> `ret` at the end lands in a different thread."

---

## Beat 3 — What I optimised · 50s

**Do**

1. Drag a window around for a few seconds.
2. Point at **System idle** — still getting CPU while you drag.
3. Point at the **Memory** and **GPU** rows.

**Say**

> "This used to take the entire processor. I measured it — over a ten-second
> drag the idle task got zero ticks and the scheduler never switched once."
>
> "Every mouse packet was redrawing the whole desktop. Eight megabytes a frame
> to move one rectangle. But the window's contents don't change while you drag
> it, only where it is. So now it's drawn once and each frame just moves those
> pixels and repaints the strip it uncovered."
>
> "Measured again after: a third of the CPU back, and you can see idle still
> getting scheduled while I'm dragging."

**Code** — `kernel/gui.c:4322`, `draw_drag_step`.

---

## Beat 4 — Locking, and the close · 25s

**Do**

1. Kernel Lab → **Two programs, one file (unlocked)** → **Write unlocked**.
2. Open `till.log` in Notepad — torn letters.
3. Kernel Lab → **Two programs, one file (locked)** → **Write with lock**.
4. Open `till.log` again — whole lines.

**Say**

> "Two programs writing one file at once. No lock, and the lines tear into each
> other. With a lock, every line comes out whole. That's the actual file — I'm
> opening it in the editor."

**Code** — `kernel/sync.c:40`, `mutex_lock`.

> "The reason to run something like this isn't that it beats Linux. It's fifteen
> thousand lines, you can read all of it, and it checks thirty-four of its own
> guarantees every time it boots. Memory isn't isolated between privilege levels
> and it's never run on real hardware — both were scope decisions, both written
> down."

---

## Timing

40s + 50s + 50s + 25s = 2:45. Fifteen seconds spare. If you overrun, drop the
last paragraph of beat 4 and finish on "every line comes out whole."

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
