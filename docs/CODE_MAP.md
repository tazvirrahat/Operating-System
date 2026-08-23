# Code map

File and line ranges against the sources as they stand in this tree. Each
block is the implementation of one demonstrated mechanism, plus a sentence on
what the mechanism is *for*. Presenter script: [`SHOWCASE.md`](SHOWCASE.md).
System-call walkthrough: [`SYSCALLS.md`](SYSCALLS.md).

Ranges are inclusive. Open the file; if a later edit has shifted a function
by a few lines, search for the symbol rather than trusting a stale number.

---

## Preemption and the scheduler tick

**What this is for.** Share one CPU among many tasks so a single loop cannot
own the machine; this is the same job Windows and Linux do with a timer
interrupt and a run queue.

| Piece | Location | Lines |
|-------|----------|-------|
| PIT programmed at 100 Hz | `kernel/kmain.c` | 72 |
| Timer ISR increments ticks and calls the registered callback | `kernel/pit.c` | 19–26, 29–38 |
| Callback installed as `task_tick` | `kernel/kmain.c` | 135 |
| Timeslice: one tick (10 ms at 100 Hz) | `kernel/task.c` | 12–18 |
| Account this task, then maybe `schedule()` | `kernel/task.c` `task_tick` | 338–385 |
| Pick the next READY task and swap stacks | `kernel/task.c` `schedule` | 252–280 |
| Register save / `esp` swap | `kernel/context.asm` `context_switch` | 22–53 |
| Round-robin, skipping idle while real work is READY | `kernel/task.c` `pick_next` | 193–219 |

IRQ acknowledgement happens *before* the handler, because a switch never
returns to the rest of `irq_handler` (`kernel/isr.c` 108–127).

---

## Idle task and Task Manager CPU

**What this is for.** When nothing is runnable, stop executing (`hlt`) instead
of spinning, and still show honest “who had the CPU” percentages — the same
role as System Idle Process / `swapper/0`.

| Piece | Location | Lines |
|-------|----------|-------|
| Idle body: `sti; hlt`, then yield if someone woke | `kernel/task.c` `idle_body` | 91–98 |
| Created as `"System idle"` | `kernel/task.c` `task_init` | 100–127 |
| Never chosen while any other task is READY | `kernel/task.c` `pick_next` | 193–219 |
| GUI parks until input or a refresh deadline | `kernel/gui.c` `gui_wait` | 4554–4565 |
| Block the caller; idle (or others) run | `kernel/task.c` `task_idle_wait` | 306–319 |
| Timed sleep used by the threads experiment | `kernel/task.c` `task_sleep` | 321–336 |
| Wake timer sleepers on the tick | `kernel/task.c` `task_tick` | 355–368 |
| Wake from keyboard/mouse/timer | `kernel/task.c` `task_idle_nudge` | 296–304 |
| Tick samples → CPU column | `kernel/gui.c` `tm_recent_pct`, `draw_tm` | 896–909, 912–1004 |
| Idle ticks drawn pale grey on the strip | `kernel/gui.c` `task_colour`, `draw_tick_strip` | 3038–3053, 3055 |

`current->ticks++` and the ring buffer of task ids live in `task_tick`
(329–330), so the GUI is reading the scheduler’s own trace, not inventing
load.

---

## Race and mutex

**What this is for.** Close the window between reading a shared value and
writing it back, so two tasks cannot lose each other’s updates — the same
reason a bank ledger or a database row takes a lock. Kernel Lab tells it as
two cashiers and one till; the primitive is still `race_run`.

| Piece | Location | Lines |
|-------|----------|-------|
| Expected total (50 × 2) | `kernel/demos.h` | 18–19 |
| Non-atomic increment, optional mutex | `kernel/demos.c` `racer` | 57–106 |
| Mutex taken/released around that increment only | `kernel/demos.c` | 60–61, 101–102 |
| Spawn two racers, wait, return the counter | `kernel/demos.c` `race_run` | 108–127 |
| `xchg` (indivisible bus lock) | `kernel/sync.h` `atomic_xchg` | 18–28 |
| Mutex: try `xchg`, yield on failure | `kernel/sync.c` `mutex_lock` | 40–47 |
| Kernel Lab till bars (red = lost deposits) | `kernel/gui.c` `draw_race_result` | 2517–2593 |
| Sidebar: Till, no lock / Till, with lock | `kernel/gui.c` `demo_entries` | 1839–1848 |
| Button runs three trials | `kernel/gui.c` `ENTRY_RACE` | 2328–2348 |

Enabling the mutex does not change the scheduler or preemption. The two
racers still run under the same 100 Hz tick; they just cannot interleave
inside the increment because `mutex_lock` is taken once per loop.

The gap is widened by spinning until a timer tick can land in it (`racer`
63–97). Totals therefore depend on preemption timing.

---

## Sequential vs overlapping threads

**What this is for.** Finish a mix of waiting and work sooner on one CPU by
running someone else's compute while a task is blocked — not by magically
parallelising arithmetic. Disk PIO in this kernel busy-waits, so it does
not count; a timer sleep does.

| Piece | Location | Lines |
|-------|----------|-------|
| Job counts (8 jobs, wait 6 ticks, spin ~3) | `kernel/demos.h` | 155–157 |
| Shared job list (wait + spin per job) | `kernel/demos.c` `threads_prepare_jobs` | 1050–1065 |
| One job: `task_sleep` then spin | `kernel/demos.c` `thread_one_job` | 1067–1076 |
| Sequential range vs two workers | `kernel/demos.c` `threads_run` | 1096–1132 |
| Sleep primitive | `kernel/task.c` `task_sleep` | 321–336 |
| Kernel Lab bars and caption | `kernel/gui.c` `draw_threads_result` | 2574–2648 |
| Sidebar: sequential / overlapping | `kernel/gui.c` | 1827–1836 |
| Run button | `kernel/gui.c` `ENTRY_THREADS` | 2346–2352 |
| Shell `threads` | `kernel/shell.c` `cmd_threads` | 297–317 |
| Self-test: overlapping < sequential | `kernel/selftest.c` | 204–215 |

---

## Semaphores and producer / consumer

**What this is for.** Bound a buffer so a fast producer cannot overrun a
slow consumer (or the reverse): print queues, thread pools, and streaming
buffers all use the same two counting semaphores.

| Piece | Location | Lines |
|-------|----------|-------|
| Four slots, 24-item batch (self-test path) | `kernel/demos.h` | 48–49 |
| `sem_wait` / `sem_post` (count, yield if zero) | `kernel/sync.c` | 55–91 |
| Producer: wait free, enqueue, post used | `kernel/demos.c` `pc_producer` | 225–255 |
| Consumer: wait used, dequeue, post free | `kernel/demos.c` `pc_consumer` | 257–281 |
| Init semaphores and spawn the pair | `kernel/demos.c` `producer_consumer_run` | 283–310 |
| Live GUI run (16 items) | `kernel/demos.c` `pc_live_start` | 750–776 |
| Live producer sets `PC_WAIT_FULL` while blocked | `kernel/demos.c` `live_producer` | 688–701 |
| Slots, in/out tags, semaphore pills, **FULL** | `kernel/gui.c` `draw_pc_viz` | 3238–3358 |
| State names (`blocked (full)`, …) | `kernel/gui.c` `pc_state_name` | 3227–3236 |
| **Start live run** | `kernel/gui.c` | 2079–2081, 2334–2344 |

---

## Deadlock

**What this is for.** Show that locks used in opposite orders can stop
progress, and that a global acquire order (or killing a waiter) breaks the
cycle — the classic reason databases order locks or run a deadlock detector.

| Piece | Location | Lines |
|-------|----------|-------|
| Task A: M1 then M2 | `kernel/demos.c` `dl_task_a` | 864–890 |
| Task B: M2 then M1, or M1 then M2 if ordered | `kernel/demos.c` `dl_task_b` | 892–933 |
| Start the pair | `kernel/demos.c` `deadlock_start` | 966–979 |
| Kill A so B can finish | `kernel/demos.c` `deadlock_kill_victim` | 981–1000 |
| Cards, lock boxes, banners | `kernel/gui.c` `draw_deadlock_viz` | 3379–3441 |
| Buttons: Trigger / Ordered locks / Kill victim | `kernel/gui.c` | 2082–2088, 2346–2364 |

Dragging a window during the wait works because only `lock_a` and `lock_b`
are blocked; the GUI task is not.

---

## MMU walk and page faults

**What this is for.** Translate virtual addresses and refuse the ones that
are not mapped, so each program can pretend it has a private, linear
memory — and a bad pointer kills that program, not the machine.

| Piece | Location | Lines |
|-------|----------|-------|
| Read CR2 (CPU-written fault address) | `kernel/paging.c` `paging_fault_address` | 30–35 |
| Identity map; page 0 left unmapped | `kernel/paging.c` `paging_init` | 37–80 |
| Walk directory → table or 4 MB page → frame | `kernel/paging.c` `paging_walk` | 209–249 |
| Print CR2 and error-code bits on #PF | `kernel/isr.c` | 80–90 |
| Kill the faulting task; keep the kernel | `kernel/isr.c` | 94–104 |
| Kernel Lab map + walk boxes | `kernel/gui.c` `draw_mmu_viz` | 3099–3224 |

Null dereference faults because `first_table[0]` is left empty (`paging_init`
46–52). Isolation between rings is *not* complete: user pages in the first
4 MB are `PAGE_USER` (comment at 55–66). Privilege (I/O, `cli`) is still
enforced by the CPU.

---

## ATA driver and persistent filesystem

**What this is for.** Turn “the file contents in RAM” into sectors on a
disk, and load them again at boot — the mechanism under every Save dialog.

| Piece | Location | Lines |
|-------|----------|-------|
| Identify the primary IDE drive | `kernel/ata.c` `ata_init` | 176–192 |
| PIO read / write one 512-byte sector | `kernel/ata.c` `ata_read`, `ata_write` | 214–234 |
| Mount or format on boot | `kernel/fs.c` `fs_init` | 367–406 |
| Superblock + table + one file’s data | `kernel/fs.c` `flush_slot` | 186–195 |
| Create/overwrite then flush | `kernel/fs.c` `fs_write` | 475–494 |

Without a drive, `fs_init` seeds a RAM-only table and the boot line says
so (`374–378`). Writes still update the in-memory files; they do not
survive power-off.

---

## Notepad save path

**What this is for.** A text editor is just another client of `fs_write`:
rename in the title field, then persist bytes under that name.

| Piece | Location | Lines |
|-------|----------|-------|
| Begin in-place rename | `kernel/gui.c` `np_begin_rename` | 1396–1410 |
| `fs_write`, then `fs_delete` the old name | `kernel/gui.c` `notepad_save` | 1438–1472 |
| Status `saved to disk` vs `saved (RAM only)` | `kernel/gui.c` | 1471 |

---

## System-call gate

**What this is for.** Let an unprivileged program request a kernel service
without giving it the run of the hardware. Detail: [`SYSCALLS.md`](SYSCALLS.md).

| Piece | Location | Lines |
|-------|----------|-------|
| Vector, register convention, `SYS_*` numbers | `kernel/syscall.h` | 1–27 |
| Dispatch on `eax` | `kernel/syscall.c` `syscall_dispatch` | 13–55 |
| IDT entry DPL 3; handler registration | `kernel/syscall.c` `syscall_init` | 57–68 |
| `iret` into ring 3 | `kernel/syscall.c` `enter_user_mode` | 70–112 |
| Gate type 0xEE vs 0x8E | `kernel/idt.h` | 15–21 |
| `idt_set_gate` | `kernel/idt.c` | 45–52 |
| Exceptions and IRQs stay DPL 0 | `kernel/idt.c` | 79–84 |
| Stub `isr128` | `kernel/isr.asm` | 104–115 |
| Ring-3 code/data descriptors | `kernel/gdt.c` | 70–71 |
| `int $0x80` wrappers | `kernel/demos.c` | 495–510 |
| Direct port I/O vs `SYS_WRITE` | `kernel/demos.c` | 518–550 |
| Drop to ring 3 | `kernel/demos.c` `user_task_entry` | 556–576 |
| `user` / `user --syscall` | `kernel/shell.c` `cmd_user` | 389–401 |
| Self-test of both paths | `kernel/selftest.c` `test_protection` | 226–254 |
