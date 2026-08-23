# How to actually record it

[`SCRIPT.md`](SCRIPT.md) is what to say. [`SHOWCASE.md`](SHOWCASE.md) is what to
click. This is everything around them — how to get the machine into the right
state, what to do in what order, and what to do when something misbehaves on
camera.

---

## The day before

**1. Build from a clean tree.**

```
dev
```

That produces `build\myos.iso`. If it errors, fix it now rather than an hour
before the deadline.

**2. Check the disk is attached.** `vmware/tazos-disk-flat.vmdk` should exist and
`MyOS.vmx` should have `ide0:0.present = "TRUE"`. Both are already true in this
repo. If the file is ever missing:

```
python3 tools/genvmdk.py
```

This matters because it decides whether the Notepad save demo can claim the file
survives a reboot. With no disk, Save says "RAM only" and you must not claim it.

**3. Do one full dry run with a stopwatch.** Not a mental rehearsal — actually
click through it. The timings in `SCRIPT.md` are targets, and the only ones that
count are yours on your machine.

**4. Decide your recorder's region.** The desktop is 1920x1080. Record the VMware
window only, not your whole screen, or the text will be unreadable when it is
scaled down.

---

## Getting the machine ready

Power on `vmware/MyOS.vmx`. Wait for the self-test to finish and the `>` prompt
to appear — this takes 30 to 50 seconds and **must not be in the take**.

At the prompt:

```
gui
```

Then set up the desktop before you start recording:

1. Start → **Task Manager**. Drag it to the right-hand side. It stays open for
   the whole recording — it is the evidence for three of the four beats.
2. Start → **Notepad**. Click the white page once so it has focus. Leave it
   open on the left.
3. Start → **Kernel Lab**. Put it in the middle.
4. Close anything else so the desktop is not cluttered.

Take a breath, check all three windows are visible and not overlapping, and
*then* start recording.

---

## The take, step by step

| # | Do this | Watch for |
|---|---|---|
| 1 | Point at **System idle** in Task Manager, Ticks climbing | The number moving is the proof |
| 2 | Kernel Lab → Background workers → **Start 3 workers** | Three `worker_` rows appear |
| 3 | Point at their Ticks — different values on each | Different numbers = time being divided |
| 4 | Kernel Lab → Hog vs Notepad → **Run with sharing OFF** | Try to type. Nothing. Wait it out |
| 5 | Click Notepad's page. **Run with sharing ON**. Click the page again. Type | Letters appear live |
| 6 | Point at `hog` in Task Manager, ticks climbing | It is still running — that is the point |
| 7 | Drag a window around for a few seconds | Point at System idle still getting CPU |
| 8 | Point at the Memory and GPU rows | Live figures, not stored ones |
| 9 | Kernel Lab → Two programs, one file (unlocked) → **Write unlocked** | |
| 10 | Open `till.log` in Notepad | Torn, interleaved letters |
| 11 | Kernel Lab → Two programs, one file (locked) → **Write with lock** | |
| 12 | Open `till.log` again | Whole lines |
| 13 | Say the closing lines and stop | |

Steps 1–3 are beat 1, 4–6 are beat 2, 7–8 are beat 3, 9–13 are beat 4.

---

## Things that will catch you out

These are all things that happened while driving this desktop, not
hypotheticals.

**The taskbar buttons move.** Focusing a window raises it, which reorders the
taskbar. The button that was third is not third any more. Read the labels; do
not click by position or muscle memory.

**Notepad ignores your typing until you click it.** Every time you come back to
it from another window, click the white page first. This is the single most
likely thing to make you stop and retake.

**The sharing-off freeze kills the mouse too.** For about three seconds nothing
responds. Do not click around trying to fix it — that is the demonstration
working. It ends by itself.

**Do not point at something a window is covering.** Windows overlap happily and
a window edge can sit over the thing you are pointing at. Separate them during
setup, not mid-take.

**The unlocked file race is a race.** Once in a while it comes out clean. Run it
again — that is honest, and it is a decent aside about nondeterminism if you
want one.

**Watch what Save says.** If Notepad reports "RAM only", the disk did not attach
and you cannot claim the file survives a reboot. Check before you record.

---

## If it goes wrong mid-take

Keep going. A retake costs three minutes; stopping and restarting the VM costs
five.

- **A demo does not fire** — click it again, and say "let me run that again"
  out loud. Nobody minds.
- **You lose your place** — the four beats are threads, preemption,
  optimisation, locking. Say the beat name and carry on from the top of it.
- **The desktop stops responding** for more than about five seconds outside the
  sharing-off demo — stop, restart the VM, start again. Do not narrate over a
  hung machine hoping it recovers.
- **You overrun three minutes** — drop the second half of beat 4 and finish on
  "each one completes." Do not rush the closing lines.

---

## Afterwards

Watch it back once before you submit, checking three things:

1. **Is the text readable** at the size it will be viewed?
2. **Does every number you spoke appear on screen**, except the two drag
   measurements you attributed by saying "I measured"?
3. **Did you say the limitations** — memory is not isolated between privilege
   levels, and it has never run on real hardware?

If the answer to the third is no, record ten seconds and append it. It is worth
more than anything you would cut to make room for it.
