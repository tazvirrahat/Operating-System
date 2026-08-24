#include "shell.h"
#include "console.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "task.h"
#include "heap.h"
#include "pit.h"
#include "demos.h"
#include "monitor.h"
#include "selftest.h"
#include "gui.h"
#include "mouse.h"
#include "pci.h"
#include "svga.h"
#include "fb.h"
#include "ata.h"
#include "fs.h"
#include "syscall.h"
#include "rtc.h"
#include "wallpaper.h"
#include "paging.h"

#include <stdint.h>
#include <stdbool.h>

#define LINE_MAX 128
#define ARGS_MAX 8

typedef void (*command_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    command_fn  fn;
} command_t;

static const command_t commands[];

/* ---- commands ----------------------------------------------------------- */

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("\n");
    for (int i = 0; commands[i].name; i++) {
        vga_set_color(VGA_LCYAN, VGA_BLACK);
        kprintf("  %-18s", commands[i].usage);
        vga_set_color(VGA_LGREY, VGA_BLACK);
        kprintf("%s\n", commands[i].help);
    }
    kprintf("\n");
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint32_t ticks = pit_ticks();
    uint32_t hz    = pit_hz();
    uint32_t secs  = hz ? ticks / hz : 0;

    kprintf("up %u:%02u:%02u  (%u timer ticks at %u Hz)\n",
            secs / 3600, (secs / 60) % 60, secs % 60, ticks, hz);
}

static void cmd_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("\n %-4s %-14s %-9s %8s %10s\n", "PID", "NAME", "STATE", "TICKS", "STACK");

    for (task_t *t = task_list(); t; t = t->next)
        kprintf(" %-4d %-14s %-9s %8u %10u\n",
                t->id, t->name, task_state_name(t->state), t->ticks, t->stack_size);

    kprintf("\n %d task%s, %u context switches, preemption %s\n\n",
            task_count(), task_count() == 1 ? "" : "s",
            task_switch_count(),
            task_preempt_enabled() ? "on" : "OFF");
}

static void cmd_meminfo(int argc, char **argv)
{
    (void)argc; (void)argv;

    heap_stats_t s;
    heap_get_stats(&s);

    kprintf("\nheap  : %08x - %08x  (%u KB)\n",
            heap_base(), heap_base() + s.total_bytes, s.total_bytes / 1024);
    kprintf("used  : %u KB in %u block%s\n",
            s.used_bytes / 1024, s.used_blocks, s.used_blocks == 1 ? "" : "s");
    kprintf("free  : %u KB in %u block%s, largest %u KB\n",
            s.free_bytes / 1024, s.free_blocks, s.free_blocks == 1 ? "" : "s",
            s.largest_free / 1024);
    kprintf("check : %s\n\n",
            heap_check() == 0 ? "structure intact" : "CORRUPT");
}

static void cmd_mmu(int argc, char **argv)
{
    uint32_t addr = 0;
    bool given = false;

    if (argc > 1) {
        const char *end;
        addr = strtoul(argv[1], &end);
        given = end && *end == '\0';
        if (!given) {
            kprintf("usage: mmu [addr]\n");
            return;
        }
    }

    kprintf("\npaging %s, %u MB identity mapped\n",
            paging_enabled() ? "on" : "OFF",
            paging_mapped_bytes() / (1024 * 1024));

    if (!given) {
        kprintf("heap   %08x  fb %08x\n", heap_base(), fb_phys_addr());
        kprintf("pass an address to walk the page tables, e.g. mmu 0x100000\n\n");
        return;
    }

    page_walk_t w;
    paging_walk(addr, &w);

    kprintf("virt %08x  dir[%u]  table[%u]\n", w.virt, w.dir_index, w.tab_index);
    kprintf("  pde %08x  %s %s %s %s\n",
            w.pde,
            (w.pde & PTE_PRESENT) ? "P" : "-",
            (w.pde & PTE_WRITE)   ? "W" : "-",
            (w.pde & PTE_USER)    ? "U" : "-",
            (w.pde & PTE_LARGE)   ? "4MB" : "4KB");

    if (w.large) {
        kprintf("  4 MB page  phys %08x\n\n", w.phys);
        return;
    }

    kprintf("  pte %08x  %s %s %s\n",
            w.pte,
            (w.pte & PTE_PRESENT) ? "P" : "-",
            (w.pte & PTE_WRITE)   ? "W" : "-",
            (w.pte & PTE_USER)    ? "U" : "-");

    if (w.present)
        kprintf("  phys %08x\n\n", w.phys);
    else
        kprintf("  not present - a load here would page-fault\n\n");
}

static void cmd_deadlock(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        deadlock_stop();
        kprintf("deadlock demo reset, %d task%s left\n",
                task_count(), task_count() == 1 ? "" : "s");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "kill") == 0) {
        deadlock_kill_victim();
        kprintf("killed lock_a; lock_b should now finish.\n");
        return;
    }

    bool ordered = argc > 1 && strcmp(argv[1], "ordered") == 0;

    deadlock_start(ordered);
    kprintf(ordered
            ? "both tasks lock M1 then M2 - they should complete.\n"
            : "opposite lock order - they will wait for each other.\n");
    kprintf("the shell stays usable. 'deadlock kill' or 'deadlock stop'.\n");
}

static void cmd_spawn(int argc, char **argv)
{
    int n = 3;

    if (argc > 1) {
        const char *end;
        n = (int)strtoul(argv[1], &end);
        if (n < 1 || n > 4) {
            kprintf("spawn: expected 1-4 tasks\n");
            return;
        }
    }

    kprintf("\nspawning %d tasks in tight loops - none of them ever yields.\n", n);
    kprintf("any interleaving below is the timer forcing a switch:\n\n  ");

    spawn_printers(n);

    kprintf("\n\ndone. %u context switches so far.\n\n", task_switch_count());
}

static void cmd_prodcons(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        pc_live_stop();
        kprintf("live producer/consumer stopped\n");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "live") == 0) {
        if (!pc_live_start()) {
            kprintf("live producer/consumer already running\n");
            return;
        }
        kprintf("live producer/consumer started - %d slots, %d items.\n",
                PC_BUFFER_SLOTS, PC_LIVE_ITEMS);
        kprintf("the shell stays usable; 'prodcons stop' to end it.\n");
        return;
    }

    kprintf("\nbounded buffer, %d slots, %d items.\n",
            PC_BUFFER_SLOTS, PC_ITEM_COUNT);
    kprintf("producer blocks when full, consumer blocks when empty -\n");
    kprintf("neither polls a flag. P = produced, c = consumed:\n\n  ");

    bool ok = producer_consumer_run(true);

    kprintf("\n\n");
    vga_set_color(ok ? VGA_LGREEN : VGA_LRED, VGA_BLACK);
    kprintf("%s\n", ok
            ? "all items received exactly once, in order, buffer never exceeded."
            : "FAILED: items lost, reordered, or the buffer overflowed.");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    kprintf("\nnotice the interleaving stays within %d of itself - that bound\n",
            PC_BUFFER_SLOTS);
    kprintf("is the semaphores holding the producer back.\n\n");
}

static void cmd_bg(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        stop_background();
        kprintf("background workers stopped, %d task%s left\n",
                task_count(), task_count() == 1 ? "" : "s");
        return;
    }

    int n = 3;
    if (argc > 1) {
        const char *end;
        n = (int)strtoul(argv[1], &end);
    }

    int started = spawn_background(n);
    if (!started) {
        kprintf("workers already running - 'bg stop' first\n");
        return;
    }

    kprintf("started %d background worker%s - they yield, so the shell stays up\n",
            started, started == 1 ? "" : "s");
    kprintf("even with 'preempt off'. try 'top', then 'bg stop'.\n");
}

static void cmd_preempt(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("preemption is currently %s\n",
                task_preempt_enabled() ? "on" : "OFF");
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        task_set_preempt(false);
        kprintf("preemption OFF - a task that never yields keeps the CPU.\n");
        kprintf("               'spawn' shows that; background workers still yield.\n");
    } else if (strcmp(argv[1], "on") == 0) {
        task_set_preempt(true);
        kprintf("preemption on - the timer will force switches again\n");
    } else {
        kprintf("usage: preempt on|off\n");
    }
}

static void cmd_preemptjob(int argc, char **argv)
{
    preempt_jobs_info_t off, on;
    uint32_t s_off, s_on, hog_off, hog_on;

    (void)argc;
    (void)argv;

    kprintf("\none hog (~%d ticks of compute) and one short job (~%d).\n",
            PREEMPT_LONG_TICKS, PREEMPT_SHORT_TICKS);
    kprintf("neither yields. preemption off: the short job waits.\n");
    kprintf("preemption on: it finishes while the hog is still running.\n\n");

    preempt_jobs_run(false);
    preempt_jobs_snapshot(&off);
    preempt_jobs_run(true);
    preempt_jobs_snapshot(&on);

    s_off   = (off.short_end > off.pair_start) ? off.short_end - off.pair_start : 0;
    s_on    = (on.short_end > on.pair_start) ? on.short_end - on.pair_start : 0;
    hog_off = (off.long_end > off.pair_start) ? off.long_end - off.pair_start : 0;
    hog_on  = (on.long_end > on.pair_start) ? on.long_end - on.pair_start : 0;

    kprintf("hogged:  short job finished in %u ticks  (hog %u)\n", s_off, hog_off);
    kprintf("sharing: short job finished in %u ticks  (hog %u)\n", s_on, hog_on);
    if (s_on && s_off && s_on < s_off)
        kprintf("the short job finished sooner because it got CPU slices.\n");
    kprintf("the hog did not get faster. total work is the same.\n\n");
}

static void cmd_hog(int argc, char **argv)
{
    desktop_hog_info_t inf;
    uint32_t ticks = DESKTOP_HOG_TICKS;
    bool sharing = true;

    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        desktop_hog_stop();
        kprintf("hog: asked to finish (only takes effect if sharing is on).\n");
        return;
    }

    desktop_hog_snapshot(&inf);
    if (inf.running) {
        kprintf("hog is already running (%s).\n",
                inf.sharing ? "sharing on" : "sharing OFF");
        return;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "off") == 0)
            sharing = false;
        else if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "start") == 0)
            sharing = true;
        else if (argv[1][0] >= '1' && argv[1][0] <= '9') {
            const char *end;
            uint32_t sec = strtoul(argv[1], &end);

            if (sec < 1)
                sec = 1;
            if (sec > 5)
                sec = 5;
            ticks = sec * 100;
            sharing = true;
        } else {
            kprintf("usage: hog start|stop|on|off|[1-5]\n");
            kprintf("  start/on/N  sharing on - type while it runs\n");
            kprintf("  off         sharing off - desktop freezes until it exits\n");
            return;
        }
    }

    if (argc >= 3 && strcmp(argv[2], "off") == 0)
        sharing = false;

    kprintf("hog: %u ticks (~%u s), sharing %s. never yields.\n",
            ticks, ticks / 100, sharing ? "on" : "OFF");
    if (sharing)
        kprintf("open Task Manager: hog Ticks climbs. type in Notepad.\n");
    else
        kprintf("desktop will freeze until the hog exits by itself.\n");

    desktop_hog_start(sharing, ticks);
}

static void cmd_filerace(int argc, char **argv)
{
    bool use_lock = false;
    file_race_info_t info;
    const fs_file_t *f;
    uint32_t i, shown;

    if (argc > 1) {
        if (strcmp(argv[1], "on") == 0)
            use_lock = true;
        else if (strcmp(argv[1], "off") != 0) {
            kprintf("usage: filerace on|off\n");
            return;
        }
    }

    kprintf("\ntwo writers, file '%s', %d records each, lock %s\n\n",
            FILE_RACE_NAME, FILE_RACE_LINES, use_lock ? "held" : "not used");

    file_race_run(use_lock);
    file_race_snapshot(&info);

    kprintf("open %s in Notepad or: cat %s\n\n", FILE_RACE_NAME, FILE_RACE_NAME);

    f = fs_find(FILE_RACE_NAME);
    if (!f || !f->data) {
        kprintf("(file missing)\n\n");
        return;
    }

    shown = f->size < 400 ? f->size : 400;
    for (i = 0; i < shown; i++) {
        char c = (char)f->data[i];
        if (c == '\n')
            kputc('\n');
        else if (c >= 32 && c < 127)
            kputc(c);
        else
            kputc('?');
    }
    if (f->size > shown)
        kprintf("\n...\n");
    kprintf("\n");
}

static void cmd_threads(int argc, char **argv)
{
    uint32_t seq, thr;

    (void)argc;
    (void)argv;

    kprintf("\n%d jobs, each wait %u ticks then spin ~%u ticks of work.\n",
            THREAD_JOBS, (uint32_t)THREAD_WAIT_TICKS,
            (uint32_t)THREAD_COMPUTE_TICKS);
    kprintf("same jobs both ways. one CPU: waits overlap, compute does not.\n\n");

    seq = threads_run(false);
    thr = threads_run(true);

    kprintf("\nsequential %u ticks, overlapping %u ticks\n", seq, thr);
    if (thr && seq && thr < seq)
        kprintf("threaded shorter because one task computed while the other slept.\n\n");
    else
        kprintf("expected overlapping < sequential; run again.\n\n");
}

static void cmd_race(int argc, char **argv)
{
    bool use_lock = false;
    int  repeats  = 3;

    if (argc > 1) {
        if (strcmp(argv[1], "on") == 0)
            use_lock = true;
        else if (strcmp(argv[1], "off") != 0) {
            kprintf("usage: race on|off [repeats]\n");
            return;
        }
    }

    if (argc > 2) {
        const char *end;
        repeats = (int)strtoul(argv[2], &end);
        if (repeats < 1 || repeats > 10)
            repeats = 3;
    }

    kprintf("\ntwo tasks, %u increments each, mutex %s\n",
            (uint32_t)RACE_ITERATIONS, use_lock ? "HELD" : "not used");
    kprintf("expected total: %u\n\n", (uint32_t)RACE_EXPECTED);

    bool all_correct = true;
    bool any_varied  = false;
    uint32_t first   = 0;

    for (int i = 0; i < repeats; i++) {
        uint32_t result = race_run(use_lock);

        bool ok = (result == RACE_EXPECTED);
        all_correct &= ok;

        if (i == 0)
            first = result;
        else if (result != first)
            any_varied = true;

        vga_set_color(ok ? VGA_LGREEN : VGA_LRED, VGA_BLACK);
        kprintf("  run %d: %-8u %s\n", i + 1, result, ok ? "" : "<- lost updates");
        vga_set_color(VGA_LGREY, VGA_BLACK);
    }

    kprintf("\n");

    if (use_lock) {
        kprintf("%s\n\n", all_correct
                ? "every run exact - the mutex closed the window."
                : "UNEXPECTED: locking should have made these exact.");
        return;
    }

    /* Report what actually happened rather than what usually happens. How many
     * updates are lost depends on where preemption lands, so identical totals
     * across a short sample are perfectly possible and should not be described
     * as variation. */
    if (all_correct) {
        kprintf("no updates lost this time. the race is timing dependent -\n");
        kprintf("try more repeats, e.g. 'race off 8'.\n\n");
    } else if (any_varied) {
        kprintf("totals differ from each other and from the expected value.\n");
        kprintf("that variation is the proof: a hardcoded fake would be\n");
        kprintf("identical every run.\n\n");
    } else {
        kprintf("updates were lost on every run. the totals happen to match\n");
        kprintf("each other here; run it again or use more repeats to see them\n");
        kprintf("diverge, since the count depends on where preemption lands.\n\n");
    }
}

static void cmd_fault(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: fault div0|opcode|gpf|null|page [addr]\n");
        kprintf("  raises a real CPU exception inside a spawned task.\n");
        kprintf("  the task is killed; the kernel and this shell survive.\n");
        kprintf("  null and page report CR2, which the CPU fills in with the\n");
        kprintf("  faulting address - a value the kernel never assigned.\n");
        return;
    }

    uint32_t addr = 0;
    if (argc > 2) {
        const char *end;
        addr = strtoul(argv[2], &end);
    }

    fault_spawn(argv[1], addr);
}

static void cmd_user(int argc, char **argv)
{
    bool use_syscall = (argc > 1 && strcmp(argv[1], "--syscall") == 0);

    if (!use_syscall) {
        kprintf("\nrunning a task in ring 3 that touches hardware directly.\n");
        kprintf("the CPU should stop it. (try 'user --syscall' for the legal route)\n");
    } else {
        kprintf("\nsame task, same privilege level, asking the kernel instead.\n");
    }

    user_mode_demo(use_syscall);
}

static void cmd_lspci_detail(const pci_device_t *d);

static void cmd_lspci(int argc, char **argv)
{
    /* With an index, show that device's base address registers. The BARs are
     * where a driver finds the hardware: the graphics driver reads them to
     * locate the registers and framebuffer it has to talk to. */
    if (argc > 1) {
        const char *end;
        int n = (int)strtoul(argv[1], &end);

        const pci_device_t *d = pci_get_device(n);
        if (!d) {
            kprintf("lspci: no device %d (there are %d)\n",
                    n, pci_device_count());
            return;
        }

        kprintf("\ndevice %d at %02x:%02x.%d\n",
                n, d->bus, d->device, d->function);
        cmd_lspci_detail(d);
        kprintf("\n");
        return;
    }

    kprintf("\n %-3s %-8s %-9s %-9s %-24s %s\n", "#",
            "ADDRESS", "VENDOR", "DEVICE", "CLASS", "IRQ");

    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_get_device(i);

        kprintf(" %-3d %02x:%02x.%d   %04x      %04x      %-24s %d\n", i,
                d->bus, d->device, d->function,
                d->vendor_id, d->device_id,
                pci_class_name(d->class_code, d->subclass),
                d->irq_line == 0xFF ? -1 : d->irq_line);
    }

    kprintf("\n %d device%s, discovered rather than hardcoded - their\n",
            pci_device_count(), pci_device_count() == 1 ? "" : "s");
    kprintf(" addresses are assigned by the firmware at boot.\n");
    kprintf(" 'lspci <n>' shows one device's base address registers.\n\n");
}

static void cmd_lspci_detail(const pci_device_t *d)
{
    kprintf("  vendor %04x device %04x  class %02x:%02x  rev %02x\n",
            d->vendor_id, d->device_id, d->class_code, d->subclass, d->revision);

    for (int i = 0; i < 6; i++) {
        if (d->bar[i] == 0)
            continue;

        kprintf("  bar%d: %08x  %s\n", i, pci_bar_address(d->bar[i]),
                pci_bar_is_io(d->bar[i]) ? "i/o ports" : "memory");
    }
}

static void cmd_gpuinfo(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("\ndisplay adapter : %s\n", svga_name());

    if (!svga_available()) {
        kprintf("\nno accelerated adapter found. the framebuffer set up by the\n");
        kprintf("firmware at boot is used instead, with every pixel written by\n");
        kprintf("the CPU. that path works on any machine, which is why it is\n");
        kprintf("the fallback.\n\n");
        kprintf("under VMware the adapter is present and this reports its\n");
        kprintf("capabilities.\n\n");
        return;
    }

    kprintf("framebuffer     : %08x\n", svga_framebuffer());
    kprintf("capabilities    : %08x\n", svga_fifo_capabilities());
    kprintf("3d capable      : %s\n",
            svga_has_3d() ? "YES" : "no - SVGA_CAP_3D (bit 14) is not set");
    kprintf("extended fifo   : %s\n",
            (svga_raw_caps() & 0x00008000) ? "yes" : "no");
    kprintf("accel fill      : %s\n", svga_can_fill() ? "yes" : "no");
    kprintf("accel copy      : %s\n", svga_can_copy() ? "yes" : "no");
    kprintf("commands issued : %u\n", svga_command_count());

    kprintf("\nfound by scanning the PCI bus, not by assuming an address.\n");
    kprintf("commands go into a queue the adapter reads, so a rectangle fill\n");
    kprintf("is six words rather than writing every pixel.\n\n");
}

static void cmd_gputest(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!fb_available()) {
        kprintf("no framebuffer; nothing to measure.\n");
        return;
    }

    if (!svga_can_fill()) {
        kprintf("\nno accelerated adapter on this machine, so there is nothing\n");
        kprintf("to compare against. run this under VMware, or QEMU with\n");
        kprintf("-vga vmware.\n\n");
        return;
    }

    const int rounds = 60;
    int w = (int)fb_width();
    int h = (int)fb_height();

    kprintf("\nfilling the whole %dx%d screen %d times, both ways.\n", w, h, rounds);
    kprintf("that is %u pixels per fill.\n\n", (uint32_t)(w * h));

    /* Software: every pixel written by the CPU, then copied to video memory.
     * This is the path used when no accelerated adapter is present. */
    uint32_t start = pit_ticks();
    for (int i = 0; i < rounds; i++) {
        fb_fill_rect(0, 0, w, h, fb_rgb((uint8_t)(i * 4), 20, 60));
        fb_present();
    }
    uint32_t cpu_ticks = pit_ticks() - start;

    /* Hardware: the same fill expressed as six words in the command queue.
     * svga_sync waits for the adapter to finish, so the timing covers the work
     * actually being done rather than just the queuing of it. */
    start = pit_ticks();
    for (int i = 0; i < rounds; i++) {
        svga_fill_rect(0, 0, w, h, fb_rgb(20, (uint8_t)(i * 4), 60));
        svga_sync();
    }
    uint32_t gpu_ticks = pit_ticks() - start;

    fb_mark_all_dirty();
    fb_present();

    kprintf("  cpu (software) : %u ticks  (%u ms)\n", cpu_ticks, cpu_ticks * 10);
    kprintf("  gpu (adapter)  : %u ticks  (%u ms)\n", gpu_ticks, gpu_ticks * 10);

    if (gpu_ticks == 0) {
        /* Faster than a 100 Hz timer can resolve. Reporting "infinitely
         * faster" would be nonsense, so state the bound the measurement
         * actually supports: it finished within one tick, so it is at least
         * as many times faster as the software path took ticks. */
        kprintf("\n  the adapter finished within a single timer tick, so this\n");
        kprintf("  only bounds it: at least %u times faster. measuring closer\n",
                cpu_ticks);
        kprintf("  would need a finer clock than 100 Hz.\n");
    } else if (cpu_ticks > gpu_ticks)
        kprintf("\n  %u times faster.\n", cpu_ticks / gpu_ticks);
    else
        kprintf("\n  no faster here - emulated adapters do the same work on the\n"
                "  host CPU, so the win is smaller than on real hardware.\n");

    kprintf("\n  the difference is what the work costs: %u pixel writes against\n",
            (uint32_t)(w * h));
    kprintf("  six words in a queue the adapter reads.\n\n");
}

static void cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("\n %-24s %8s  %s\n", "NAME", "SIZE", "MODIFIED");

    for (int i = 0; i < FS_MAX_FILES; i++) {
        const fs_file_t *f = fs_at(i);
        if (!f)
            continue;

        kprintf(" %-24s %8u  %02u:%02u %02u %s %u\n",
                f->name, f->size, f->hour, f->minute,
                f->day, rtc_month_name(f->month), f->year);
    }

    kprintf("\n %d file%s, %u bytes used\n\n",
            fs_file_count(), fs_file_count() == 1 ? "" : "s", fs_bytes_used());
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: cat <file>\n");
        return;
    }

    const fs_file_t *f = fs_find(argv[1]);
    if (!f) {
        kprintf("cat: %s: no such file\n", argv[1]);
        return;
    }

    kprintf("\n");
    for (uint32_t i = 0; i < f->size; i++)
        kputc((char)f->data[i]);
    kprintf("\n");
}

static void cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: write <file> <text...>\n");
        kprintf("  replaces the file's contents. use 'append' to add to it.\n");
        return;
    }

    /* Rebuild the text from the tokens, since the tokeniser split on spaces
     * and the words are what the user typed. */
    char buf[512];
    int  n = 0;

    for (int i = 2; i < argc && n < (int)sizeof(buf) - 2; i++) {
        const char *w = argv[i];
        while (*w && n < (int)sizeof(buf) - 2)
            buf[n++] = *w++;
        if (i + 1 < argc)
            buf[n++] = ' ';
    }
    buf[n++] = '\n';

    if (fs_write(argv[1], buf, (uint32_t)n))
        kprintf("wrote %d bytes to %s%s\n", n, argv[1],
                fs_on_disk() ? " (on disk)" : " (RAM only)");
    else
        kprintf("write: failed (%s)\n",
                fs_error()[0] ? fs_error() : "table full, or file too large");
}

static void cmd_append(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: append <file> <text...>\n");
        return;
    }

    char buf[512];
    int  n = 0;

    for (int i = 2; i < argc && n < (int)sizeof(buf) - 2; i++) {
        const char *w = argv[i];
        while (*w && n < (int)sizeof(buf) - 2)
            buf[n++] = *w++;
        if (i + 1 < argc)
            buf[n++] = ' ';
    }
    buf[n++] = '\n';

    if (fs_append(argv[1], buf, (uint32_t)n))
        kprintf("appended %d bytes to %s%s\n", n, argv[1],
                fs_on_disk() ? " (on disk)" : " (RAM only)");
    else
        kprintf("append: failed (%s)\n",
                fs_error()[0] ? fs_error() : "table full, or file too large");
}

static void cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: rm <file>\n");
        return;
    }

    if (fs_delete(argv[1]))
        kprintf("removed %s\n", argv[1]);
    else if (fs_error()[0])
        kprintf("rm: %s: %s\n", argv[1], fs_error());
    else
        kprintf("rm: %s: no such file\n", argv[1]);
}

static void cmd_syscalls(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("system calls serviced since boot: %u\n", syscall_count());
}

static void cmd_disk(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kprintf("\n");
    if (!ata_present()) {
        kprintf("  drive     : none on primary IDE (0x1F0)\n");
        kprintf("  filesystem: %s\n\n", fs_boot_state());
        return;
    }

    kprintf("  model     : %s\n", ata_model()[0] ? ata_model() : "(unnamed)");
    kprintf("  sectors   : %u (%u MB)\n", ata_sectors(), ata_sectors() / 2048u);
    kprintf("  filesystem: %s, %d file%s, %u bytes\n",
            fs_boot_state(), fs_file_count(),
            fs_file_count() == 1 ? "" : "s", fs_bytes_used());
    kprintf("  writes    : %u sectors\n\n", ata_writes());
}

static void cmd_wallpaper(int argc, char **argv)
{
    if (argc > 1) {
        const char *end;
        int n = (int)strtoul(argv[1], &end);

        if (n < 1 || n > WALLPAPER_COUNT) {
            kprintf("wallpaper: pick 1-%d\n", WALLPAPER_COUNT);
            return;
        }

        wallpaper_set(n - 1);
    } else {
        wallpaper_next();
    }

    kprintf("wallpaper: %s (%d of %d)\n",
            wallpaper_name(wallpaper_current()),
            wallpaper_current() + 1, WALLPAPER_COUNT);
    kprintf("visible in graphical mode - run 'gui'\n");
}

static void cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;

    rtc_time_t now;
    rtc_read(&now);

    kprintf("%02u:%02u:%02u  %u %s %u\n",
            now.hour, now.minute, now.second,
            now.day, rtc_month_name(now.month), now.year);
}

static void cmd_gui(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!mouse_present())
        kprintf("\nno ps/2 mouse detected - the gui will run, but without a cursor.\n");

    kprintf("\nswitching to graphics mode. press ESC to come back.\n");

    gui_run();

    kprintf("\nback in text mode.\n\n");
}

static void cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;

    vga_clear();
    monitor_run();
    vga_clear();
}

static void cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    selftest_run();
    kprintf("\n");
}

static void cmd_demo(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("\n");
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("[1/5] preemptive multitasking\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    kprintf("      3 tasks, tight loops, no yields:\n\n  ");
    spawn_printers(3);
    kprintf("\n\n      ^ the timer forced every one of those switches.\n\n");

    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("[2/5] ablation: the same thing with preemption disabled\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    task_set_preempt(false);
    kprintf("      preemption OFF. one task should monopolise the CPU:\n\n  ");
    spawn_printers(3);
    task_set_preempt(true);
    kprintf("\n\n      ^ grouped, not interleaved. preemption was doing the work.\n\n");

    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("[3/5] race condition, no lock\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    for (int i = 0; i < 3; i++) {
        uint32_t r = race_run(false);
        kprintf("      run %d: %-8u expected %u %s\n", i + 1, r,
                (uint32_t)RACE_EXPECTED, r == RACE_EXPECTED ? "" : "<- WRONG");
    }

    kprintf("\n");
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("[4/5] same workload, mutex held\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    for (int i = 0; i < 3; i++) {
        uint32_t r = race_run(true);
        kprintf("      run %d: %-8u expected %u %s\n", i + 1, r,
                (uint32_t)RACE_EXPECTED, r == RACE_EXPECTED ? "OK" : "<- WRONG");
    }

    kprintf("\n");
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("[5/5] kernel state\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    kprintf("\n");
    monitor_draw();
    kprintf("\n");
}

/* ---- command table ------------------------------------------------------ */

static const command_t commands[] = {
    { "help",     "help",             "list these commands",                       cmd_help     },
    { "demo",     "demo",             "scripted walkthrough of every feature",     cmd_demo     },
    { "selftest", "selftest",         "run all verification checks",               cmd_selftest },
    { "spawn",    "spawn [1-4]",      "run N tasks that never yield",              cmd_spawn    },
    { "bg",       "bg [1-4] | stop",  "background workers, shell stays usable",    cmd_bg       },
    { "preempt",  "preempt on|off",   "toggle timer preemption (ablation test)",   cmd_preempt  },
    { "hog",      "hog start|stop|on|off|[1-5]", "CPU hog: freeze Notepad (off) or type while it runs (on)", cmd_hog },
    { "preemptjob","preemptjob",      "short job vs hog, with and without sharing", cmd_preemptjob },
    { "race",     "race on|off [n]",  "shared counter with and without a mutex",   cmd_race     },
    { "filerace", "filerace on|off",  "two writers, one file, torn vs clean lines", cmd_filerace },
    { "prodcons", "prodcons [live]",  "bounded buffer with counting semaphores",   cmd_prodcons },
    { "deadlock", "deadlock [ordered|kill|stop]", "circular wait, or the ordered fix", cmd_deadlock },
    { "threads",  "threads",          "same jobs sequential vs two overlapping tasks", cmd_threads  },
    { "fault",    "fault <kind>",     "raise a real CPU exception in a task",      cmd_fault    },
    { "user",     "user [--syscall]", "run a task in ring 3 (privilege demo)",     cmd_user     },
    { "tasks",    "tasks",            "list tasks and their CPU time",             cmd_tasks    },
    { "gui",      "gui",              "graphical mode: windows, mouse, terminal",  cmd_gui      },
    { "top",      "top",              "live kernel monitor",                       cmd_top      },
    { "meminfo",  "meminfo",          "heap usage and block list state",           cmd_meminfo  },
    { "mmu",      "mmu [addr]",       "walk the page tables for an address",       cmd_mmu      },
    { "lspci",    "lspci [n]",        "devices found on the pci bus",              cmd_lspci    },
    { "gpuinfo",  "gpuinfo",          "graphics adapter and acceleration",         cmd_gpuinfo  },
    { "gputest",  "gputest",          "benchmark cpu fills against gpu fills",      cmd_gputest  },
    { "ls",       "ls",               "list files",                                cmd_ls       },
    { "cat",      "cat <file>",       "print a file",                              cmd_cat      },
    { "write",    "write <f> <text>", "replace a file's contents",                 cmd_write    },
    { "append",   "append <f> <txt>", "add to a file",                             cmd_append   },
    { "rm",       "rm <file>",        "delete a file",                             cmd_rm       },
    { "syscalls", "syscalls",         "how many system calls the kernel has serviced", cmd_syscalls },
    { "disk",     "disk",             "ATA drive, filesystem state, writes",       cmd_disk     },
    { "wallpaper","wallpaper [1-6]",  "change the desktop background",             cmd_wallpaper},
    { "date",     "date",             "wall-clock time from the cmos chip",        cmd_date     },
    { "uptime",   "uptime",           "time since boot, from timer ticks",         cmd_uptime   },
    { "echo",     "echo <text>",      "print arguments",                           cmd_echo     },
    { "clear",    "clear",            "clear the screen",                          cmd_clear    },
    { 0, 0, 0, 0 },
};

/* ---- line editing and dispatch ------------------------------------------ */

static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < ARGS_MAX) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';

        if (!*p)
            break;

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t')
            p++;
    }

    return argc;
}

void shell_dispatch(char *line)
{
    char *argv[ARGS_MAX];
    int argc = tokenize(line, argv);

    if (argc == 0)
        return;

    for (int i = 0; commands[i].name; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].fn(argc, argv);
            return;
        }
    }

    kprintf("unknown command: %s (try 'help')\n", argv[0]);
}

static void prompt(void)
{
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    kprintf("> ");
    vga_set_color(VGA_LGREY, VGA_BLACK);
}

/* ---- command history ---------------------------------------------------- */

#define HISTORY_MAX 16

static char history[HISTORY_MAX][LINE_MAX];
static int  history_count;      /* entries stored, saturating at HISTORY_MAX */
static int  history_next;       /* where the next entry goes (circular) */

static void history_add(const char *line)
{
    if (!line[0])
        return;

    /* Skip consecutive duplicates: repeating a command should not fill the
     * history with copies of it. */
    if (history_count > 0) {
        int last = (history_next - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (strcmp(history[last], line) == 0)
            return;
    }

    strncpy(history[history_next], line, LINE_MAX - 1);
    history[history_next][LINE_MAX - 1] = '\0';

    history_next = (history_next + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX)
        history_count++;
}

/* Fetch the entry `back` steps into the past, where 1 is the most recent.
 * Returns NULL when `back` runs off the end of what we have kept. */
static const char *history_get(int back)
{
    if (back < 1 || back > history_count)
        return 0;

    int index = (history_next - back + HISTORY_MAX * 2) % HISTORY_MAX;
    return history[index];
}

/* Erase the current input and print a different line in its place. Backspace
 * only moves the cursor left, so each character has to be overwritten with a
 * space and then backed over a second time. */
static void replace_line(char *line, int *len, const char *replacement)
{
    for (int i = 0; i < *len; i++)
        kprintf("\b \b");

    *len = 0;

    if (!replacement)
        return;

    while (replacement[*len] && *len < LINE_MAX - 1) {
        line[*len] = replacement[*len];
        kputc(line[*len]);
        (*len)++;
    }

    line[*len] = '\0';
}

void shell_run(void)
{
    char line[LINE_MAX];
    int  len = 0;

    /* How far back in history the user has scrolled; 0 means "editing a fresh
     * line", which is why the down arrow can return to an empty prompt. */
    int  browsing = 0;

    kprintf("\ntype 'help' for commands, or 'demo' for a guided tour.\n");
    kprintf("use the up and down arrows to recall previous commands.\n\n");
    prompt();

    for (;;) {
        char c = kbd_getchar();

        if (c == '\n') {
            kputc('\n');
            line[len] = '\0';

            history_add(line);
            shell_dispatch(line);

            len = 0;
            browsing = 0;
            prompt();
            continue;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                kputc('\b');
                kputc(' ');
                kputc('\b');
            }
            continue;
        }

        if (c == KEY_UP) {
            const char *entry = history_get(browsing + 1);
            if (entry) {
                browsing++;
                replace_line(line, &len, entry);
            }
            continue;
        }

        if (c == KEY_DOWN) {
            if (browsing > 1) {
                browsing--;
                replace_line(line, &len, history_get(browsing));
            } else if (browsing == 1) {
                /* Stepping forward past the newest entry returns to the empty
                 * line the user was originally typing. */
                browsing = 0;
                replace_line(line, &len, 0);
            }
            continue;
        }

        /* Scrollback. A screenful at a time, like a terminal. */
        if (c == KEY_PGUP) {
            vga_scroll_back(20);
            continue;
        }

        if (c == KEY_PGDN) {
            vga_scroll_back(-20);
            continue;
        }

        /* Left and right are decoded by the driver but the line editor only
         * supports appending, so they are ignored rather than inserted as
         * stray control characters. */
        if (c == KEY_LEFT || c == KEY_RIGHT)
            continue;

        if (len < LINE_MAX - 1) {
            line[len++] = c;
            kputc(c);
        }
    }
}
