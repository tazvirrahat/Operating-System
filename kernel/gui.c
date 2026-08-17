#include "gui.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "keyboard.h"
#include "console.h"
#include "shell.h"
#include "string.h"
#include "task.h"
#include "heap.h"
#include "pit.h"

#include <stdint.h>
#include <stdbool.h>

/* ---- layout --------------------------------------------------------------
 *
 * Everything scales from the framebuffer size reported at boot rather than
 * being hardcoded, because GRUB gives us the closest mode it can rather than
 * exactly what was asked for.
 */

#define TASKBAR_H   48
#define TITLE_H     32
#define BORDER      2
#define TEXT_SCALE  2
#define CELL_W      (8 * TEXT_SCALE)
#define CELL_H      (8 * TEXT_SCALE)

#define MAX_WINDOWS 2

typedef enum { WIN_TERMINAL, WIN_STATS } window_kind_t;

typedef struct {
    int           x, y, w, h;
    const char   *title;
    window_kind_t kind;
    bool          visible;
} window_t;

static window_t windows[MAX_WINDOWS];
static int      window_count;
static int      focused;

static int dragging = -1;
static int drag_dx, drag_dy;

static uint32_t col_desktop, col_face, col_edge, col_shade;
static uint32_t col_title_on, col_title_off, col_term_bg, col_term_fg;
static uint32_t col_bar, col_bar_btn, col_bar_btn_on, col_white, col_black;

/* ---- terminal ------------------------------------------------------------ */

#define TERM_COLS 72
#define TERM_ROWS 34

static char term_cells[TERM_ROWS][TERM_COLS];
static int  term_cx, term_cy;

static void term_clear(void)
{
    memset(term_cells, ' ', sizeof(term_cells));
    term_cx = term_cy = 0;
}

static void term_scroll(void)
{
    memmove(term_cells[0], term_cells[1], (TERM_ROWS - 1) * TERM_COLS);
    memset(term_cells[TERM_ROWS - 1], ' ', TERM_COLS);
    term_cy = TERM_ROWS - 1;
}

static void term_putc(char c)
{
    if (c == '\n')      { term_cx = 0; term_cy++; }
    else if (c == '\r') { term_cx = 0; }
    else if (c == '\t') { term_cx = (term_cx + 4) & ~3; }
    else if (c == '\b') {
        if (term_cx > 0) term_cells[term_cy][--term_cx] = ' ';
    } else if (c >= 32) {
        term_cells[term_cy][term_cx++] = c;
    }

    if (term_cx >= TERM_COLS) { term_cx = 0; term_cy++; }
    if (term_cy >= TERM_ROWS) term_scroll();
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_window_frame(const window_t *win, bool active)
{
    fb_fill_rect(win->x + 6, win->y + 6, win->w, win->h, col_shade);

    fb_fill_rect(win->x, win->y, win->w, win->h, col_face);
    fb_rect(win->x, win->y, win->w, win->h, col_black);

    fb_fill_rect(win->x + BORDER, win->y + BORDER,
                 win->w - 2 * BORDER, TITLE_H,
                 active ? col_title_on : col_title_off);

    fb_text(win->x + 12, win->y + 8, win->title, col_white, TEXT_SCALE);

    /* Close box, drawn but not wired to anything: the taskbar toggles
     * visibility instead, and a decoration that looks clickable and is not
     * would be worse than none. */
    int bx = win->x + win->w - 34;
    fb_rect(bx, win->y + 8, 18, 16, col_white);
}

static void draw_terminal(const window_t *win, bool active)
{
    int tx = win->x + BORDER + 6;
    int ty = win->y + TITLE_H + BORDER + 6;

    fb_fill_rect(win->x + BORDER, win->y + TITLE_H + BORDER,
                 win->w - 2 * BORDER, win->h - TITLE_H - 2 * BORDER,
                 col_term_bg);

    for (int row = 0; row < TERM_ROWS; row++)
        for (int col = 0; col < TERM_COLS; col++) {
            char c = term_cells[row][col];
            if (c != ' ')
                fb_char(tx + col * CELL_W, ty + row * CELL_H,
                        c, col_term_fg, TEXT_SCALE);
        }

    if (active)
        fb_fill_rect(tx + term_cx * CELL_W, ty + term_cy * CELL_H + CELL_H - 3,
                     CELL_W, 3, col_term_fg);
}

/* Render an unsigned value as text. There is no snprintf here. */
static void draw_number(int x, int y, uint32_t v, uint32_t colour)
{
    char buf[12];
    int  p = 0;

    if (v == 0)
        buf[p++] = '0';
    while (v > 0) {
        buf[p++] = (char)('0' + (v % 10));
        v /= 10;
    }

    char out[12];
    int  q = 0;
    while (p > 0)
        out[q++] = buf[--p];
    out[q] = '\0';

    fb_text(x, y, out, colour, TEXT_SCALE);
}

static void draw_stats(const window_t *win)
{
    heap_stats_t heap;
    heap_get_stats(&heap);

    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;

    static const char *labels[5] = {
        "tasks", "uptime s", "heap KB", "switches", "mouse pkts"
    };
    uint32_t values[5] = {
        (uint32_t)task_count(), secs, heap.used_bytes / 1024,
        task_switch_count(), mouse_packet_count()
    };

    int tx = win->x + 16;
    int ty = win->y + TITLE_H + 16;

    for (int i = 0; i < 5; i++) {
        fb_text(tx, ty + i * (CELL_H + 8), labels[i], col_black, TEXT_SCALE);
        draw_number(tx + 190, ty + i * (CELL_H + 8), values[i], col_title_on);
    }
}

static void draw_taskbar(void)
{
    int bar_y = (int)fb_height() - TASKBAR_H;

    fb_fill_rect(0, bar_y, (int)fb_width(), TASKBAR_H, col_bar);
    fb_fill_rect(0, bar_y, (int)fb_width(), 2, col_edge);

    fb_text(16, bar_y + 16, "MyOS", col_white, TEXT_SCALE);

    /* One button per window, highlighted while that window is visible. */
    for (int i = 0; i < window_count; i++) {
        int bx = 140 + i * 220;

        fb_fill_rect(bx, bar_y + 8, 200, TASKBAR_H - 16,
                     windows[i].visible ? col_bar_btn_on : col_bar_btn);
        fb_rect(bx, bar_y + 8, 200, TASKBAR_H - 16, col_black);

        fb_text(bx + 14, bar_y + 16, windows[i].title, col_white, TEXT_SCALE);
    }

    fb_text((int)fb_width() - 420, bar_y + 16,
            "ESC returns to console", col_white, TEXT_SCALE);
}

static void draw_cursor(void)
{
    int mx = mouse_x();
    int my = mouse_y();

    /* Scaled up: an 8-pixel arrow is invisible at this resolution. */
    const int s = 2;

    for (int i = 0; i < 16; i++) {
        fb_fill_rect(mx, my + i * s, s, s, col_black);
        if (i > 0 && i < 13)
            fb_fill_rect(mx + s, my + i * s, s, s, col_white);
        if (i > 1 && i < 11)
            fb_fill_rect(mx + 2 * s, my + i * s, s, s, col_white);
        if (i > 2 && i < 9)
            fb_fill_rect(mx + 3 * s, my + i * s, s, s, col_white);
    }

    for (int i = 0; i < 8; i++)
        fb_fill_rect(mx + (4 + i) * s, my + (5 + i) * s, s, s,
                     i < 5 ? col_white : col_black);
}

/* ---- input --------------------------------------------------------------- */

static bool in_rect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void raise_window(int index)
{
    if (index < 0 || index == window_count - 1)
        return;

    window_t tmp = windows[index];
    for (int i = index; i < window_count - 1; i++)
        windows[i] = windows[i + 1];
    windows[window_count - 1] = tmp;
}

static bool handle_click(int mx, int my)
{
    int bar_y = (int)fb_height() - TASKBAR_H;

    /* Taskbar first: it sits above everything. */
    if (my >= bar_y) {
        for (int i = 0; i < window_count; i++) {
            int bx = 140 + i * 220;
            if (in_rect(mx, my, bx, bar_y + 8, 200, TASKBAR_H - 16)) {
                windows[i].visible = !windows[i].visible;
                if (windows[i].visible) {
                    raise_window(i);
                    focused = window_count - 1;
                }
                return true;
            }
        }
        return true;
    }

    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i].visible)
            continue;
        if (!in_rect(mx, my, windows[i].x, windows[i].y, windows[i].w, windows[i].h))
            continue;

        bool on_title = in_rect(mx, my, windows[i].x, windows[i].y,
                                windows[i].w, TITLE_H + BORDER);

        raise_window(i);
        focused = window_count - 1;

        if (on_title) {
            dragging = focused;
            drag_dx  = mx - windows[dragging].x;
            drag_dy  = my - windows[dragging].y;
        }
        return true;
    }

    return true;
}

/* ---- entry point --------------------------------------------------------- */

void gui_run(void)
{
    char line[128];
    int  len = 0;

    if (!fb_available()) {
        kprintf("graphics mode is not available on this machine.\n");
        return;
    }

    col_desktop    = fb_rgb(32, 60, 96);
    col_face       = fb_rgb(200, 200, 205);
    col_edge       = fb_rgb(245, 245, 250);
    col_shade      = fb_rgb(18, 32, 52);
    col_title_on   = fb_rgb(28, 88, 168);
    col_title_off  = fb_rgb(120, 120, 130);
    col_term_bg    = fb_rgb(14, 16, 24);
    col_term_fg    = fb_rgb(120, 230, 140);
    col_bar        = fb_rgb(38, 44, 58);
    col_bar_btn    = fb_rgb(60, 68, 86);
    col_bar_btn_on = fb_rgb(28, 88, 168);
    col_white      = fb_rgb(255, 255, 255);
    col_black      = fb_rgb(0, 0, 0);

    int W = (int)fb_width();
    int H = (int)fb_height();

    term_clear();

    window_count = 2;
    windows[0] = (window_t){ W - 460, 90, 400, 260,
                             "System", WIN_STATS, true };
    windows[1] = (window_t){ 80, 120,
                             TERM_COLS * CELL_W + 2 * BORDER + 12,
                             TERM_ROWS * CELL_H + TITLE_H + 2 * BORDER + 12,
                             "Terminal", WIN_TERMINAL, true };
    focused = 1;

    mouse_set_bounds(W, H);

    /* Shell output now lands in the terminal window rather than the console. */
    console_set_sink(term_putc);

    kprintf("MyOS graphical mode - %dx%d\n", W, H);
    kprintf("the taskbar toggles windows; drag by the title bar.\n");
    kprintf("try: help, tasks, meminfo, race off 5\n\n> ");

    bool     dirty = true;
    int      last_mx = mouse_x(), last_my = mouse_y();
    uint32_t last_frame = pit_ticks();

    for (;;) {
        while (kbd_available()) {
            char c = kbd_poll();

            if (c == 27) {
                console_set_sink(0);

                /* Repaint the text console from its own stored contents; it
                 * was never touched while the GUI was up. */
                fbcon_clear();
                return;
            }

            if (c == '\n') {
                kputc('\n');
                line[len] = '\0';
                shell_dispatch(line);
                len = 0;
                kprintf("> ");
            } else if (c == '\b') {
                if (len > 0) { len--; kputc('\b'); }
            } else if (c >= 32 && c < 127 && len < (int)sizeof(line) - 1) {
                line[len++] = c;
                kputc(c);
            }

            dirty = true;
        }

        if (mouse_take_click(MOUSE_LEFT))
            dirty = handle_click(mouse_x(), mouse_y()) || dirty;

        if (dragging >= 0) {
            if (mouse_buttons() & MOUSE_LEFT) {
                windows[dragging].x = mouse_x() - drag_dx;
                windows[dragging].y = mouse_y() - drag_dy;
                dirty = true;
            } else {
                dragging = -1;
            }
        }

        if (mouse_x() != last_mx || mouse_y() != last_my) {
            last_mx = mouse_x();
            last_my = mouse_y();
            dirty = true;
        }

        if (pit_ticks() - last_frame >= 25)
            dirty = true;

        if (!dirty) {
            task_yield();
            continue;
        }

        dirty = false;
        last_frame = pit_ticks();

        fb_fill_rect(0, 0, W, H - TASKBAR_H, col_desktop);
        fb_text(24, 24, "MyOS", col_white, 3);
        fb_text(24, 64, "bare metal x86 - no operating system underneath",
                col_edge, TEXT_SCALE);

        for (int i = 0; i < window_count; i++) {
            if (!windows[i].visible)
                continue;

            draw_window_frame(&windows[i], i == focused);

            if (windows[i].kind == WIN_TERMINAL)
                draw_terminal(&windows[i], i == focused);
            else
                draw_stats(&windows[i]);
        }

        draw_taskbar();
        draw_cursor();

        fb_present();

        task_yield();
    }
}
