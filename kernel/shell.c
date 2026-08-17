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
    (void)argc; (void)argv;

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

    spawn_background(n);
    kprintf("started %d background worker%s - the shell is still responsive.\n",
            n, n == 1 ? "" : "s");
    kprintf("try 'top' to watch them, then 'bg stop'.\n");
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
        kprintf("preemption OFF - a running task will now keep the CPU\n");
        kprintf("               indefinitely. try 'spawn 3' to see it break.\n");
    } else if (strcmp(argv[1], "on") == 0) {
        task_set_preempt(true);
        kprintf("preemption on - the timer will force switches again\n");
    } else {
        kprintf("usage: preempt on|off\n");
    }
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
    { "race",     "race on|off [n]",  "shared counter with and without a mutex",   cmd_race     },
    { "prodcons", "prodcons",         "bounded buffer with counting semaphores",   cmd_prodcons },
    { "fault",    "fault <kind>",     "raise a real CPU exception in a task",      cmd_fault    },
    { "user",     "user [--syscall]", "run a task in ring 3 (privilege demo)",     cmd_user     },
    { "tasks",    "tasks",            "list tasks and their CPU time",             cmd_tasks    },
    { "top",      "top",              "live kernel monitor",                       cmd_top      },
    { "meminfo",  "meminfo",          "heap usage and block list state",           cmd_meminfo  },
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

static void dispatch(char *line)
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
            dispatch(line);

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
