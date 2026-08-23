# TazOS — click script

Boot. At `>` type `gui`. Then do these in order.

---

## Idle

- **What it is:** When nothing is busy, the CPU sleeps. It is not spinning in a loop.
- **What you will see:** Task Manager, a row named **System idle**, its Ticks number moving.
- **Clicks:** Start → **Task Manager**. Leave it open. Point at **System idle**.
- **What to say:** “The clock still ticks. The processor is halted until work arrives.”

---

## Hog vs Notepad (sharing)

- **What it is:** A hog is a program that burns the CPU on purpose and never waits. Sharing is whether the timer lets anyone else run.
- **What you will see:** Sharing off: mouse and Notepad freeze ~3 seconds, then come back. Sharing on: you type in Notepad while the hog is still listed in Task Manager.
- **Clicks:**
  1. Start → **Notepad**. Click the white page.
  2. Start → **Kernel Lab**. Click **Hog vs Notepad**.
  3. Click **Run with sharing OFF**. Try to type in Notepad. Wait. The desktop dies for about 3 seconds, then the hog exits by itself.
  4. Click the Notepad page. Click **Run with sharing ON**. Click the Notepad page again. Type. Letters appear live.
  5. Start → **Task Manager**. Find **hog**. Ticks goes up. That is the thread using the CPU.
- **What to say:** “Off: one program steals the processor; Notepad cannot run. On: the same hog, but the timer gives Notepad slices. The hog is not faster. Notepad just gets a turn.”

Keys mashed during the freeze may appear when it unfreezes, or not. Either is fine.

---

## A thread is running

- **What it is:** A thread is a program the scheduler knows about. Ticks is how much CPU it has had.
- **What you will see:** Extra rows in Task Manager. Their Ticks numbers climb.
- **Clicks:** Kernel Lab → **Background workers** → **Start 3 workers**. Start → **Task Manager**. Watch **worker_** Ticks.
- **What to say:** “Those three are running. They wait their turn, so Notepad still works. The freeze demo was Hog vs Notepad, not these.”

Stop them when you are done: **Stop the workers**.

---

## Two programs, one file

- **What it is:** Two programs both writing `till.log` at the same time — like two Notepads saving at once.
- **What you will see:** Unlocked: mixed letters (`ABAB…`). Locked: whole lines of A then B. Same file, open it yourself.
- **Clicks:**
  1. Kernel Lab → **Two programs, one file (unlocked)** → **Write unlocked**.
  2. Start → **Notepad** (or File Explorer) → open **till.log**. Torn letters.
  3. Kernel Lab → **Two programs, one file (locked)** → **Write with lock**.
  4. Open **till.log** again. Whole lines.
- **What to say:** “Two writers, one file. Without a lock the letters tear. With a lock each line is complete. This is the real filesystem, not a cartoon.”

---

## Threads (wait overlapping)

- **What it is:** Two programs on one CPU. Waiting can overlap; pure computing cannot.
- **What you will see:** Sequential bar longer than overlapping bar.
- **Clicks:** **Threads, sequential** → Run. Then **Threads, overlapping** → Run. Point at the two times.
- **What to say:** “One CPU. The win is overlapping a wait with someone else’s work, not two computes at once.”

---

## Producer / consumer

- **What it is:** A four-slot queue. The writer stops when it is full instead of overwriting.
- **What you will see:** Boxes fill. Then **FULL**. The producer waits.
- **Clicks:** **Producer / consumer** → **Start live run**. Wait until a box says the producer is waiting because it is full.
- **What to say:** “Four slots. It waits. It does not smash the old items.”

---

## Deadlock

- **What it is:** Two programs, two locks, opposite order. Each has one and wants the other.
- **What you will see:** Those two freeze. You can still drag this window.
- **Clicks:** **Deadlock** → **Trigger deadlock**. Drag Kernel Lab. Then **Ordered locks** (or **Kill victim**).
- **What to say:** “They are stuck on each other. The rest of the machine still runs.”

---

## System call

- **What it is:** User programs cannot touch hardware. They ask the kernel through a door (`int 0x80`).
- **What you will see:** `user --syscall` prints and exits. `user` dies; the kernel lives.
- **Clicks:** Click the **Terminal**. Type `user --syscall`. Then `user`.
- **What to say:** “Restricted mode cannot poke hardware. The legal door is a system call. The kernel survives the illegal one.”

---

## Notepad save (disk)

- **What it is:** Save writes the file on disk (or only RAM if this guest has no disk).
- **What you will see:** File Explorer lists `hello.txt`. After reboot it is still there — only if Save did not say RAM only.
- **Clicks:** Start → **Notepad**. Type. Name `hello.txt`. **Save**. Start → **File Explorer**. Point at `hello.txt`.
- **What to say:** “That is a real file. If it says RAM only, do not claim a reboot.”

---

## If something stalls

| What you see | What to do |
|--------------|------------|
| Sharing OFF freezes the mouse ~3 s | That is the demo. Wait. The hog exits by itself. |
| Sharing ON and Notepad ignores keys | Click the white page so Notepad has focus, then type. |
| Unlocked till.log has no torn lines | Run unlocked again. |
| Notepad says RAM only | No disk. Do not claim a reboot. |

---

## Pictures

![Hog vs Notepad — Task Manager ticks](images/hog-tm.png)

![till.log torn without a lock](images/filerace-torn.png)

![till.log intact with a lock](images/filerace-clean.png)
