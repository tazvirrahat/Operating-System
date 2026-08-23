# TazOS — three-minute recording script

Four beats. Every number spoken is either on screen live or was measured on this
machine; none of it is illustrative. Where a figure comes from an earlier
measurement rather than the screen, the script says "measured" out loud.

**Before recording**

- Boot and reach `>`. Type `gui`. The self-test is a third of the budget, so it
  does not go in the take.
- Open **Task Manager** (Start → Task Manager) and drag it to the right so it is
  visible the whole time. It is the evidence for three of the four beats.
- Open **Notepad**, click the white page once so it has focus, then leave it.
- Have **Kernel Lab** open (Start → Kernel Lab).
- Close everything else.

---

## Beat 1 — A thread is a real thing the kernel schedules *(40 s)*

**DO**

1. Point at Task Manager. Note **System idle** and its climbing **Ticks**.
2. Kernel Lab → **Background workers** → **Start 3 workers**.
3. Point at the three new `worker_` rows, Ticks climbing on each.

**SAY**

> "This is Task Manager reading the kernel's own scheduler, live. Right now the
> busiest thing on the machine is the idle task — when there is no work, the
> processor is halted, not spinning."

> "I start three worker threads. They appear immediately, and each one's tick
> count climbs. That number is CPU time the scheduler actually gave them. Three
> threads, one processor, and the desktop stays responsive throughout."

**Leave the workers running** — they make the next beat more convincing.

---

## Beat 2 — Why preemption is the thing that matters *(50 s)*

**DO**

1. Kernel Lab → **Hog vs Notepad** → **Run with sharing OFF**.
2. Immediately try to type in Notepad. Nothing happens. The desktop freezes for
   about three seconds, then returns on its own.
3. Click the Notepad page. Click **Run with sharing ON**. Click the Notepad page
   again and type — letters appear while the hog is still running.
4. Point at Task Manager: `hog` is listed, ticks climbing.

**SAY**

> "Now a program that deliberately never gives the CPU back. With sharing off,
> it takes the machine — the mouse stops, Notepad will not accept a key. One
> badly behaved program has frozen everything."

> "Same program, sharing on. It is still running, still burning CPU — you can
> see it in Task Manager — but I can type. The timer interrupt takes the
> processor away from it a hundred times a second and hands it to something
> else."

> "That is the difference between a machine one program can hang and one it
> cannot. It is the single most important thing in the project."

---

## Beat 3 — The optimisations, with the numbers *(50 s)*

**DO**

1. Drag a window around by its title bar — smoothly, several seconds.
2. Point at Task Manager's **System idle** row while dragging: it keeps getting
   CPU.
3. Point at the **Memory** and **GPU** rows underneath.

**SAY**

> "Dragging this used to take the whole processor. I measured it: over a
> ten-second drag the idle task received zero ticks and the scheduler did not
> switch once — the renderer had the machine to itself."

> "Every mouse packet was redrawing the entire desktop: eight megabytes a frame
> to move one rectangle. But a window's contents do not change while you drag
> it — only where they are. So it is rasterised once, and each frame moves those
> pixels and repaints only the strip it uncovered. That is what a real
> compositor does."

> "Measured again afterwards, the idle task gets a third of the CPU back, and
> you can see it still getting scheduled while I drag."

*(If asked for more: the pointer used to blink because the display copied the
bounding box of everything that changed, so a fast flick became a full-width
copy. It now copies the rectangles themselves — four kilobytes however fast you
move.)*

---

## Beat 4 — Why you would choose it, and what it costs *(25 s)*

**DO**

1. Kernel Lab → **Two programs, one file (unlocked)** → **Write unlocked**.
2. Open `till.log` in Notepad — torn, interleaved letters.
3. Kernel Lab → **Two programs, one file (locked)** → **Write with lock**.
4. Open `till.log` again — whole lines.

**SAY**

> "Two programs writing one file at once. Without a lock the lines tear into
> each other. With a lock, each one completes. That is the real filesystem — I
> am opening the actual file in the editor."

> "The reason to run something like this is not that it beats Linux. It is that
> the whole system is fifteen thousand lines, every mechanism is visible, and it
> checks thirty-four of its own guarantees at every single boot. Memory is not
> isolated between privilege levels and it has never run on real hardware —
> both were scope decisions, and both are written down."

---

## Timing

| Beat | Length | Running |
|---|---|---|
| 1 — Threads | 40 s | 0:40 |
| 2 — Preemption | 50 s | 1:30 |
| 3 — Optimisations | 50 s | 2:20 |
| 4 — Locking and the close | 25 s | 2:45 |

Fifteen seconds of headroom. If you overrun, drop the second half of beat 4 and
end on "each one completes."

---

## What is real, and what to say about it

Your instructor asked for real data. Everything above is:

| Claim | Where it comes from |
|---|---|
| Idle task ticks, worker ticks, hog ticks | Task Manager, live, read from the scheduler |
| Memory used / total | Live, from the heap allocator |
| GPU commands issued | Live, counted by the driver |
| "Zero idle ticks during a drag" | Measured over a ten-second drag before the fix |
| "A third of the CPU back" | Measured the same way after it |
| "Eight megabytes a frame" | 1920 x 1080 x 4 bytes — arithmetic, not an estimate |
| "Thirty-four checks" | The boot self-test tally |
| `till.log` torn vs whole | The actual file, opened in the editor |

Say **"I measured"** for the two drag figures. It is accurate, and it is the
phrase that separates a number from a guess.

---

## Cut for time

These are in [`SHOWCASE.md`](SHOWCASE.md) if a longer cut is ever wanted:
deadlock and recovery, the producer/consumer queue, threads sequential versus
overlapping, ring 3 and `int 0x80`, the MMU walk, and the GPU acceleration
result that measured slower and was left switched off.
