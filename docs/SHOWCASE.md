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

That is the only file you cut to. Two more are listed in
[`CODE_MAP.md`](CODE_MAP.md) if a question needs them:
`kernel/context.asm:23` for the context switch itself, and
`kernel/gui.c:4336` for the drag optimisation.

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

## Part 1 — Proving the threads are real · 45s

Two separate claims here, and they need different evidence.

**That they are threads** — separate execution contexts, not one loop pretending
— is proved by the stack column and by killing one.

**That they are scheduled** — genuinely preempted rather than run one after
another — is proved by the interleaving vanishing when the timer is off.

**Do:** point at **System idle** in Task Manager. Say this *before* starting
anything — at rest idle sits around 85%, and that is the point.

> "This is Task Manager reading my scheduler live. The busiest thing on the
> machine is the idle task — no work to do, so the processor is halted."

**Do:** Kernel Lab → **Background workers** → **Start 3 workers**. Point at the
**Memory** column.

> "Three threads. And look at the memory column — eight kilobytes each. That's a
> separate stack per thread, allocated off the heap, and every one has a guard
> word at the bottom that the kernel checks on every boot. That's what makes
> them threads and not just three function calls."

**Do:** select one worker, click **End task**.

> "And I can kill one of them. The other two carry on — separate lifetimes,
> separate stacks."

**Do:** Kernel Lab → **Preemption proof** → run it.

> "Now, are they actually being scheduled? Three more tasks, each printing its
> own letter in a tight loop, none of them ever yielding."
>
> "A, B and C, all mixed together. Something stopped each one mid-loop and let
> the next one run — and it resumed exactly where it left off. That's the stack
> doing its job."

**Do:** **Preemption switch** → OFF. Run **Preemption proof** again.

> "Same three tasks, timer off."
>
> "All the A's, then the B's, then the C's. Four context switches that time,
> instead of forty-one."

**Do:** turn preemption back **ON**, then cut to `kernel/task.c:348`.

> "And back on. That's preemption — it runs off the timer interrupt, and every
> tick it decides whether to switch."

---

## Part 2 — Why that matters · 30s

Part 1 proved the scheduler works. This is what it buys you.

**Do:** Kernel Lab → **Hog vs Notepad** → **Run with sharing OFF**. Try to type
in Notepad.

> "Same idea, but now it's a program that grabs the CPU and never lets go — and
> the scheduler is off."
>
> "Nothing. The mouse is gone too. One bad program has taken the whole machine."

**Do:** wait for it to release. It runs for ten seconds — use the time, try the
mouse, try typing again.

**Do:** click Notepad's page. **Run with sharing ON**. Click the page and type.

> "Same program, scheduler on. I can type — and it's still running, still
> burning CPU, right there in Task Manager."
>
> "That's the difference between a machine one bad program can freeze, and one
> it can't."

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

493 spoken words. At 150 words a minute that is 3:17 of talking, plus roughly
twenty seconds of silence while the demos run, the hog holds the machine, and
you drag. That is **over three minutes** — deliberately, because part 1 grew to
carry two separate proofs.

Pick one of these before you record:

- **Talk at 170 and keep everything.** 493 words is 2:54, plus the silences puts
  you a shade over. Tight but doable if you are a fast talker.
- **Drop the "End task" step in part 1** (25 words, about 10 seconds with the
  clicking). The stack column still carries the thread claim on its own.
- **Drop part 3** and mention the drag optimisation in one sentence instead. It
  is the least load-bearing of the four.

Read it aloud with a stopwatch once. Whichever you cut, cut it before the take,
not during.

---

## Four things that will trip you up

- **Taskbar buttons move.** Focusing a window reorders them. Read the labels,
  don't click by position.
- **Notepad ignores keys until you click its page.** Every time you come back to
  it. This is the most likely reason you'll need a retake.
- **Sharing OFF kills the mouse too**, for ten seconds. That's the demo working.
  Don't click around — it releases on its own.
- **The unlocked race is a race.** Now and then it comes out clean. Run it again.
- **The letters appear inside Kernel Lab**, in the dark output console at the
  bottom of its right-hand pane — not in the Terminal. You do not need Terminal
  visible for part 1.

If Notepad's Save ever says "RAM only", the disk didn't attach — don't claim the
file survives a reboot.

---

## Showing that apps call down into the kernel

This is the layering question, and it is a different one from privilege rings.
Notepad does not write to the disk. It does not allocate memory. It asks the
kernel, and the kernel's layers do the work:

```
notepad_save()          the app
  -> fs_write()         filesystem: names, sizes, timestamps
    -> kmalloc()        heap: finds room for the contents
    -> ata_write()      driver: LBA addressing, ATA command
      -> outsw()        the actual port I/O to the drive
```

You can prove the whole chain with one counter, because `ata_write` increments a
sector count that nothing else touches.

**Do:** in the Terminal, type `disk`. Note the **writes** figure.

**Do:** in Notepad, type something, name it `hello.txt`, press **Save**.

**Do:** back in the Terminal, `disk` again.

> "Before I saved, the driver had written two thousand three hundred and
> fifty-eight sectors. I press Save in Notepad — and now it's ten more, and the
> file count has gone up by one."
>
> "Notepad didn't do any of that. It called the filesystem, the filesystem asked
> the heap for room and handed the bytes to the disk driver, and the driver did
> the port I/O. Four layers, and you can watch the bottom one move."

Measured on this build with a disk attached: 2358 sectors before, 2368 after,
five files becoming six.

**Costs about 25 seconds.** If you add it, it is a better close than part 4's
last paragraph — drop that and finish here.

If the guest has no disk, `disk` reports none and the counter does not exist. The
heap half of the chain still works: `meminfo` before and after a large save shows
the allocation. Do not claim sectors you cannot show.

---

## Part 5 — System calls · 30s

Notepad's **Save** goes through the system call gate. It does not call the
filesystem directly — it puts the call number in `eax`, the filename and bytes
in `ebx`/`ecx`/`edx`, and executes `int 0x80`. The kernel takes the interrupt,
lands in the dispatcher, and reaches the filesystem from there.

The kernel counts every system call it services, so you can prove the trap
happened rather than assert it.

**Do:** in the Terminal, type `syscalls`. Note the number.

**Do:** in Notepad, type something and press **Save**.

**Do:** back in the Terminal, `syscalls` again.

> "Notepad is about to save a file. Before it does, the kernel has serviced
> seven system calls."
>
> "I press Save — and now it's eight. That save didn't call the filesystem
> directly. It put the call number in a register and executed `int 0x80`, the
> processor took the interrupt, and the kernel did the write on Notepad's
> behalf."

**Optional, if there is time —** the privilege side, two clicks in Kernel Lab:
**Ring 3: touch hardware** then **Ring 3: via syscall**.

> "And here's why that gate exists. Same unprivileged program, two routes. The
> first goes straight for a hardware port and the processor kills it. The second
> asks through the gate, and it works."

Measured on this build: counter 7 before the save, 8 after, and the file appears
in `ls` with the right size and timestamp.

**Be precise about the scope.** Only the save is routed this way. The rest of the
desktop is kernel code calling kernel functions directly, and if you are asked,
say so:

> "One service is routed through the gate, not the whole desktop. These apps are
> part of the kernel — making all of them user programs would need a process
> model, user-space binaries and a loader, which I scoped out."

**Costs 30 seconds.** If you add it, drop part 3 — see Timing.

---

## Say "I measured"

Everything you quote is on screen live except the two drag figures — zero idle
ticks before, a third of the CPU back after. Those came from a measured run, so
say so. It's the phrase that separates a number from a guess.
