# Demo Guide — VMware setup and showcase script

Two parts: getting the kernel running in VMware, then what to actually show and say.

> **Verified.** The kernel has been booted in VMware Workstation Pro and all 30 self-tests passed, including ring 3 and paging. The full boot transcript is saved at [`vmware-boot-log.txt`](vmware-boot-log.txt). The QEMU path in Part 4 also works, as a fallback.

---

## Part 1 — VMware setup

### Step 1: Build the ISO

From the project folder in PowerShell:

```powershell
.\dev.cmd
```

This produces `build\myos.iso`. That single file is the whole operating system — everything else in the repo is source code used to make it.

### Step 2: Install VMware

Download **VMware Workstation** from Broadcom's site. Check the current licensing terms on the download page — personal and educational use has been free, but that has changed hands and is worth confirming rather than assuming.

VirtualBox is a perfectly good substitute if VMware gives you trouble; the steps are nearly identical, and it is a smaller download.

### Step 3: Create the virtual machine

**The reliable path** — build it through the wizard rather than opening the supplied `.vmx`:

1. **File → New Virtual Machine**
2. Choose **Typical**
3. Select **Installer disc image file (iso)** and browse to `build\myos.iso`
4. VMware will say it *"cannot detect which operating system is in this disc image"*. **This is expected and correct** — your OS is not one it knows about. Click Next.
5. Guest operating system: **Other**, version: **Other**
6. Name it `MyOS`, pick any location
7. Disk size: leave the default — you will remove it in a moment
8. Click **Customize Hardware** (or edit settings after creating):
   - **Memory:** 128 MB is plenty
   - **Processors:** 1
   - **Hard Disk:** select it and click **Remove** — the kernel has no disk driver and cannot write anything, so this makes the VM provably harmless
   - **Network Adapter:** remove (no network stack)
   - **USB / Sound / Printer:** remove
9. **Finish**, then **Power on**

**The alternative path:** open `vmware\MyOS.vmx` directly through *File → Open*. It is pre-configured with no disk and a serial log. If VMware complains about the file or the relative ISO path, use the wizard path above instead — the `.vmx` is a convenience, not a requirement.

### Step 4: What you should see

1. A GRUB menu flashes past (about 2 seconds)
2. The MyOS banner, then subsystem initialisation lines
3. **The self-test runs automatically** — around 20 lines of `[PASS]`, taking roughly 30–50 seconds
4. `30 passed, 0 failed`
5. The `>` prompt

**Click inside the VM window to give it your keyboard. Press `Ctrl+Alt` to release it back to Windows.** This catches people out — if typing does nothing, you have not clicked in.

### Step 5: If it does not boot

| Symptom | Likely cause |
|---|---|
| Black screen, nothing at all | ISO not attached, or the CD-ROM is not set to connect at power-on |
| GRUB appears but errors | ISO built incorrectly — rebuild with `.\dev.cmd` |
| Boots then immediately reboots in a loop | A triple fault. Would be a genuine VMware-specific bug — tell me and I will fix it |
| Reaches the prompt but typing does nothing | You have not clicked into the VM window |

---

## Part 2 — Recording

**Windows has a screen recorder built in:** press `Win + Alt + R` to start and stop. It saves to `Videos\Captures`. Good enough, and nothing to install.

**OBS Studio** is free and better if you want to crop to just the VMware window or record a voiceover.

**Practical advice:**

- **Do a full practice run before recording.** Know what you are typing next.
- **Skip the boot self-test in the final cut.** It is 30–50 seconds of watching. Either start recording after the prompt appears, or trim it — but do show the `30 passed, 0 failed` line, because it is strong.
- **Type `clear` between sections** so each demo starts on a clean screen.
- **Aim for 2–3 minutes.** Nobody watches longer.
- Record at the VM's native size. Text stays crisp; scaled-up text does not.

---

## Part 3 — What to show, and what to say

The timed script for a three-minute recording comes first. Everything after it is the full catalogue, for a longer cut or for answering questions.

Each section in the catalogue is one demo: the command, what appears, and the one sentence that explains why it matters. **The sentence is the important part** — the output alone does not make the point.

### The three-minute script

Five beats. Everything below is a command you type and a sentence you say over it; the sentence is the point, because the output on its own does not make an argument.

Do one dry run with a stopwatch before recording. The timings below are targets, not measurements — how long each demo takes depends on the host, and the only number that matters is the one you get.

**Before you start recording:** boot the machine and let it reach the prompt. The boot self-test is 30–50 seconds of watching text scroll, which is a third of your budget. Either start recording once the prompt is up, or record it and cut it down to the last line in editing.

---

**0:00 — Open on the verdict** *(~10 s)*

```
selftest
```

Let it run in the background of your intro if you like, or just scroll up to the tally you already have from boot.

> "Thirty checks across every subsystem, passing. They do not read back what the kernel printed — each one either turns a mechanism off to prove it was load-bearing, or reads a value the CPU wrote."

---

**0:10 — Preemption, and proof that it is real** *(~45 s)*

```
spawn 3
```

Three tasks print interleaved.

> "Three tasks running at once. None of them yields — there is no cooperation in this code. The timer interrupt takes the CPU away from whichever one is running and hands it to the next."

Now the part that makes it evidence rather than a claim:

```
preempt off
spawn 3
```

> "Same three tasks, with the timer no longer driving the scheduler. They run one after another to completion. The interleaving was the scheduler, not the order I happened to print things in."

```
preempt on
```

**This is the strongest thirty seconds in the video.** Anyone can print from three tasks. Turning the mechanism off and showing the behaviour disappear is the difference between a demo and a proof.

---

**0:55 — A race condition, then the fix** *(~35 s)*

```
race off
```

The final count is wrong, and it is a different wrong number each time.

> "Two tasks incrementing one counter without a lock. The read, the add and the write are three separate instructions, and preemption can land between them, so updates get lost. Notice the answer changes between runs — that is genuine nondeterminism, not a fixed bug."

```
race on
```

> "Same two tasks, same iteration count, now taking a mutex. Exactly the expected total, every time."

---

**1:30 — Privilege separation** *(~30 s)*

```
user
```

The task is killed by the CPU; the shell keeps running.

> "A task running in ring 3. It tried a privileged instruction, and the processor refused and raised a fault. The kernel caught it, killed that task, and carried on — that is the containment, and it is enforced by hardware, not by checking."

```
user --syscall
```

> "The same task going through `int 0x80` instead. That is the one interrupt gate in the whole table with a descriptor privilege level of 3, and it is the only door from user code into the kernel."

---

**2:00 — The desktop, and two subsystems talking** *(~60 s)*

```
gui
```

Let the desktop appear. Drag a window by its title bar. Then, in the terminal window:

```
write demo.txt the scheduler is driven by the timer interrupt
```

Click **File Explorer** on the taskbar. The file is there, with its size and the time you wrote it. Click it and the contents appear in the preview.

> "Same kernel — the shell is now running inside a window it is also drawing. There is no graphics library underneath: the kernel asks GRUB for a linear framebuffer, and every pixel, every glyph and the mouse pointer come from code in this repository."

> "The file manager holds no copy of the filesystem. It walks it every frame, which is why the file I just wrote in the terminal is already in the window — nothing had to tell it."

Finish with **Start → Change wallpaper** if you have a spare five seconds. It is small, but it shows the desktop is a live thing rather than a picture.

---

**Closing line**

> "No operating system underneath any of this. It boots from GRUB, sets up its own descriptor tables, drives the hardware directly, manages its own memory, and draws its own desktop."

---

### What to leave out, and why

- **`demo`** — it is the guided tour of everything, and it is far longer than three minutes. Good for a longer cut, wrong for this one.
- **`gputest`** — the 2D acceleration benchmark is real and worth showing in a longer video, but explaining what a command FIFO is costs more time than the result is worth here.
- **3D acceleration** — the driver is written and the adapter advertises the capability, but it reports a 3D hardware version of zero, so the pipeline is not available to this guest. Do not claim it works. If asked, the honest answer is a good one: the driver declines rather than pushing commands into a FIFO that will not execute them.
- **Booting on real hardware** — never attempted. Say so if asked.

### If something goes wrong on camera

Nothing here needs to go right the first time. Every command is repeatable and none of them can leave the kernel in a bad state — that is the point of the fault containment. If a task dies unexpectedly, `tasks` will show you what is still running, and the shell will still be there.


---

### 1. Preemptive multitasking ★

```
> spawn 3
```

Three tasks print their own letter in tight loops. The output interleaves: `AAAABBBBCCCCAAAAA…`

> "These three tasks are in infinite loops. Not one of them ever asks to stop or gives up the CPU. The hardware timer interrupts them and my scheduler takes the processor away — that interleaving is the only visible evidence of it."

### 2. Ablation — proving it is real ★★

**This is the strongest thing in the whole project.** Show it immediately after the previous one.

```
> preempt off
> spawn 3
```

Now one task runs to completion before the next starts — grouped, not interleaved.

```
> preempt on
> spawn 3
```

Interleaving returns.

> "Anyone can print letters in a pattern. So let me turn preemption off and run exactly the same code. Now the first task keeps the CPU until it finishes and the others never get a look in — which is exactly what theory predicts. Turn it back on and switching resumes. That failure is the proof."

Why this lands: it is the difference between *claiming* something works and *demonstrating* that the system breaks correctly without it.

### 3. Race condition, then the fix ★

```
> race off 5
```

Five runs, all wrong, all different: `93, 78, 84, 63, 89` where 100 is expected.

```
> race on 5
```

Five runs, all exactly 100.

> "Two tasks each increment a shared counter fifty times. Without a lock the total comes out wrong — and differently wrong every run, because the answer depends on exactly when the timer interrupts. That variation is the point: a hardcoded fake would print the same number every time. With a mutex, it is exact."

### 4. Producer/consumer

```
> prodcons
```

Output batches into groups of four: `P1 P2 P3 P4 c1 c2 c3 c4 P5 P6 P7 P8…`

> "A four-slot buffer between a producer and a consumer, using counting semaphores. Watch the producer get exactly four ahead and then stop — it blocks because the buffer is full, and the consumer blocks when it is empty. Neither one polls a flag; the semaphores put them to sleep and wake them."

### 5. Privilege separation ★

```
> user
```

The task is killed by a general protection fault.

```
> user --syscall
```

The same task succeeds.

> "This task runs in ring 3, unprivileged. First it writes to a hardware port directly — and the CPU kills it. I did not check for that in software; the processor refused. Then the same task asks the kernel through a system call instead, and it works. Same code, same privilege level, opposite outcome, decided by hardware."

### 6. Fault handling with hardware evidence

```
> fault page 0xdeadb000
```

```
cr2 (faulting address): deadb000
```

> "I just typed that address, and the CPU reported it back to me from control register 2 — a register my kernel never writes. And notice the shell is still running: the faulting task was killed, the kernel survived. That is what a segfault actually is, seen from the kernel's side."

Try it with different addresses to show `cr2` following. `fault null` gives `cr2: 00000000`.

### 7. Live kernel monitor

```
> bg 4
> top
```

A live table of tasks, tick counts, heap usage and interrupt counters, refreshing continuously. Press any key to exit.

> "This reads straight out of the scheduler's task list while it runs. Four background workers, and you can watch their CPU time climb in real time. The shell stayed responsive the whole time they were running — which only works because the shell gets preempted too."

Then `bg stop` to end them.

### 8. Memory allocator

```
> meminfo
```

> "A heap allocator I wrote — first-fit, it splits blocks when they are too big and merges neighbouring free blocks back together. Every block header carries a magic value so that overflowing one block into the next is detectable instead of silent."

### 9. The self-test

```
> selftest
```

30 checks across every subsystem.

> "The kernel testing itself. Thirty checks — and importantly they do not just read back what the kernel printed. Each one either turns a mechanism off to prove it was load-bearing, or reads a value the CPU wrote."

---

### 10. The graphical desktop ★★

```
> gui
```

A 1920x1080 desktop: draggable windows, a taskbar with a working clock, a Start menu, and a terminal you can keep using. Drag a window by its title bar, scroll the terminal with the mouse wheel, and open **Start → Change wallpaper** a couple of times.

> "This is the same kernel — the shell is now running inside a window it is also drawing. There is no graphics library underneath: the kernel asks GRUB for a linear framebuffer, and every pixel, every glyph and the mouse pointer are written by code in this repository."

If asked why it does not lag: only the rectangle that changed is copied to the screen. Redrawing all 8.3 MB of the frame on every mouse movement was the first version, and it was unusable.

### 11. The filesystem

```
> write notes.txt hello from my kernel
> ls
> cat notes.txt
> append notes.txt and a second line
> cat notes.txt
> rm notes.txt
> ls
```

Then click **File Explorer** on the taskbar and click through the files. The one you just wrote is there, with its size and the time you wrote it.

> "A namespace mapping names to contents, with the storage allocated and released as files grow and are deleted. It is in memory rather than on a disk — there is no disk driver in this kernel, and the header says so. What is missing is the block layer underneath; everything that makes it a filesystem is here."

> "The file manager holds no copy of any of this. It walks the filesystem every frame, which is why a file written in the terminal is already in the window — nothing had to tell it."

This is the strongest single moment in the demo, because it shows two subsystems you wrote talking to each other through a third.

Expect to be asked whether files survive a reboot. They do not, and the honest answer is better than a hedge: persistence needs an ATA driver and an on-disk format, which is a block layer rather than a filesystem concept.

### Closing line

> "No operating system underneath any of this. It boots from GRUB, sets up its own descriptor tables, drives the hardware directly, manages its own memory, and draws its own desktop."

---

## Part 4 — QEMU fallback

If VMware causes trouble, this already works and is equally valid:

```powershell
.\dev.cmd run
```

Every command above behaves identically. QEMU is an emulator rather than a virtualiser, which is a distinction nobody watching will care about — and if asked, "I developed it against QEMU and it runs the same in a VM" is a perfectly good answer.

You also already have static screenshots in `docs/images/` and an animated GIF at the top of the README, so there is a fallback behind the fallback.

---

## Questions you should expect

**"Did you write all of this yourself?"** — Answer honestly. What matters is that you can explain any part of it, which is why the challenges document is worth reading closely.

**"What is the hardest part?"** — The context switch, honestly. A task is really just a stack; switching means saving registers, swapping the stack pointer, and popping the other task's registers back. The `ret` at the end returns somewhere completely different from where you called.

**"Why not 64-bit?"** — 64-bit mode requires paging to be configured before you can print a single character, which forces the hardest component first, blind. Same concepts, worse order for learning.

**"Does it have a GUI / can it browse the web?"** — No, deliberately. A GUI needs a USB stack for mouse input. A browser needs a USB stack, then a network driver, then TCP/IP, then TLS, then HTML, CSS and JavaScript engines. Knowing why that chain makes it infeasible is more useful than a broken attempt at it.

**"What does not work?"** — Memory is not isolated between kernel and user tasks; only instruction privilege is enforced. System call arguments are not validated. The scheduler has no priorities. Say these plainly — stated limitations read as competence, discovered ones do not.
