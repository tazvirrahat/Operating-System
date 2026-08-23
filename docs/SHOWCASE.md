# TazOS — click script

Boot it, type `gui` at the `>` prompt, and work down this list in order.

Each section tells you what the thing actually is, what you should expect to see
on screen, which buttons to press, and roughly what to say over it. There is
also a note on how to capture it as a picture, because a still that shows a
contrast is worth more than a paragraph explaining one.

---

## Idle

**What it is.** When there is nothing to do, the CPU sleeps. It is not sitting
in a loop burning cycles waiting for something to happen.

**What you will see.** Task Manager, a row called **System idle**, and its Ticks
number going up.

**Clicks.** Start → **Task Manager**. Leave it open — you will want it for most
of what follows. Point at **System idle**.

**What to say.** "The clock is still ticking. The processor is halted until
there is work for it."

**Visual idea.** This is the one feature with no contrast shot yet, and it needs
one, because "nothing is happening" is hard to photograph. Take two Task Manager
captures: one at rest with idle Ticks racing ahead, and one with the three
workers running where idle barely moves. Side by side, the same row tells the
whole story.

---

## Hog vs Notepad (sharing)

**What it is.** A hog is a program that burns the CPU on purpose and never
waits its turn. Sharing is whether the timer lets anybody else get a look in.

**What you will see.** Sharing off: the mouse and Notepad lock up for about
three seconds, then everything comes back. Sharing on: you can type in Notepad
while the hog is still sitting there in Task Manager.

**Clicks.**

1. Start → **Notepad**. Click the white page.
2. Start → **Kernel Lab**. Click **Hog vs Notepad**.
3. Click **Run with sharing OFF**. Try typing in Notepad. Wait. The desktop dies
   for about three seconds and then the hog exits on its own.
4. Click the Notepad page. Click **Run with sharing ON**. Click the Notepad page
   again and type. The letters show up as you go.
5. Start → **Task Manager**. Find **hog**. Its Ticks are climbing — that is the
   thread eating the CPU.

**What to say.** "Off: one program takes the processor and Notepad never gets a
turn. On: same hog, but the timer hands Notepad slices anyway. The hog has not
got slower. Notepad has just been given a share."

If you mash keys during the freeze they may all appear when it unfreezes, or
they may not. Either is fine.

**Visual idea.** You already have this one working — `preempt-hogged.png` and
`preempt-sharing.png` are the pair, and `hog-notepad-live.png` catches the
moment typing works. It is the template everything else here is trying to copy:
the same screen twice, one thing changed, and the difference is obvious without
a caption.

---

## A thread is running

**What it is.** A thread is a program the scheduler knows about. Ticks is how
much CPU it has been given.

**What you will see.** Extra rows in Task Manager with their Ticks numbers
going up.

**Clicks.** Kernel Lab → **Background workers** → **Start 3 workers**. Then
Start → **Task Manager** and watch the **worker_** rows.

**What to say.** "Those three are running right now. They take turns, so Notepad
still works. The freeze earlier was the Hog vs Notepad demo, not these."

Click **Stop the workers** when you are done.

**Visual idea.** One capture with all three `worker_` rows visible and their
Ticks at different values — different numbers on the same screen prove the
scheduler is dividing time, not just listing processes. `hog-tm.png` already
does this for the hog.

---

## Two programs, one file

**What it is.** Two programs both writing `till.log` at the same time, like two
copies of Notepad saving at once.

**What you will see.** Unlocked, the letters come out mixed together
(`ABAB…`). Locked, you get whole lines of A followed by whole lines of B. Same
file both times — go and open it yourself.

**Clicks.**

1. Kernel Lab → **Two programs, one file (unlocked)** → **Write unlocked**.
2. Start → **Notepad** (or File Explorer) → open **till.log**. Torn letters.
3. Kernel Lab → **Two programs, one file (locked)** → **Write with lock**.
4. Open **till.log** again. Whole lines.

**What to say.** "Two writers, one file. Without a lock the letters tear into
each other. With a lock every line comes out complete. That is the real
filesystem, not a mock-up."

**Visual idea.** `filerace-torn.png` and `filerace-clean.png` are the strongest
pair in the project after the preemption ones, for the same reason: it is the
same file in the same editor and only one thing changed. Crop both to the same
region so the eye lands on the text and nothing else.

---

## Threads (wait overlapping)

**What it is.** Two programs on one CPU. Waiting can overlap. Actual computing
cannot.

**What you will see.** The sequential bar is longer than the overlapping one.

**Clicks.** **Threads, sequential** → Run. Then **Threads, overlapping** → Run.
Point at the two times.

**What to say.** "One CPU. The win comes from overlapping a wait with somebody
else's work, not from doing two sums at once."

**Visual idea.** `threads-run1.png` and `threads-run2.png` cover this. The
number that matters is the elapsed time, so if you retake them, get both bars
and both times into a single frame — a reader comparing two separate images has
to hold a number in their head, and they will not bother.

---

## Producer / consumer

**What it is.** A queue with four slots. The writer stops when it is full
instead of trampling what is already there.

**What you will see.** The boxes fill up, then one says **FULL**, and the
producer waits.

**Clicks.** **Producer / consumer** → **Start live run**. Wait for a box to
report that the producer is waiting because the queue is full.

**What to say.** "Four slots. It waits. It does not stamp on the old items."

**Visual idea.** No capture of this yet. The shot to get is the exact moment
the queue reads FULL and the producer is marked as waiting — that single frame
is the whole idea. It is also the hardest to time, so take several and keep the
clearest.

---

## Deadlock

**What it is.** Two programs, two locks, grabbed in opposite orders. Each one is
holding what the other one wants.

**What you will see.** Those two stop dead. You can still drag this window
around.

**Clicks.** **Deadlock** → **Trigger deadlock**. Drag the Kernel Lab window to
prove the machine is fine. Then **Ordered locks**, or **Kill victim**.

**What to say.** "Those two are stuck on each other. Everything else carries on."

**Visual idea.** Also missing a capture. The convincing frame is the two blocked
rows in Task Manager *while* the Kernel Lab window is mid-drag somewhere it
obviously was not before — that proves the rest of the system is alive, which is
the point. A still of two frozen rows on its own just looks like a hang.

---

## System call

**What it is.** User programs are not allowed to touch hardware. They have to
ask the kernel through a door, which is `int 0x80`.

**What you will see.** `user --syscall` prints its output and exits normally.
`user` gets killed, and the kernel keeps going.

**Clicks.** Click the **Terminal**. Type `user --syscall`, then `user`.

**What to say.** "Restricted mode cannot poke at hardware. The legal way in is a
system call. The illegal one gets the program killed and the kernel survives it."

**Visual idea.** `ring3-syscall.png` has this. If you retake it, get both
commands and both outcomes in one scroll-back — the contrast only works when the
success and the failure are visible together.

---

## Notepad save (disk)

**What it is.** Save writes the file to disk, or only to RAM if this guest has
no disk attached.

**What you will see.** File Explorer listing `hello.txt`. It survives a reboot —
but only if Save did not tell you it was RAM only.

**Clicks.** Start → **Notepad**. Type something. Name it `hello.txt`. **Save**.
Then Start → **File Explorer** and point at `hello.txt`.

**What to say.** "That is a real file. If it says RAM only, do not claim it
survives a reboot."

**Visual idea.** No capture yet. Put Notepad and File Explorer on screen at the
same time with the same filename visible in both, so one frame shows the file
being written and the file being listed. If you do have a disk attached, the
much better shot is File Explorer after a reboot with the file still in it.

---

## If something stalls

| What you see | What to do |
|--------------|------------|
| Sharing OFF freezes the mouse for ~3 s | That is the demo. Wait. The hog exits by itself. |
| Sharing ON and Notepad ignores keys | Click the white page to give Notepad focus, then type. |
| Unlocked till.log has no torn lines | Run unlocked again. |
| Notepad says RAM only | No disk attached. Do not claim a reboot. |

---

## Pictures

![Hog vs Notepad — Task Manager ticks](images/hog-tm.png)

![till.log torn without a lock](images/filerace-torn.png)

![till.log intact with a lock](images/filerace-clean.png)
