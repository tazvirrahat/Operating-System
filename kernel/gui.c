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

/* ---- layout ---------------------------------------------------------------
 *
 * Nothing here is a fixed pixel position. GRUB gives whatever mode the
 * firmware actually offers, which has ranged from 640x480 to 1920x1080 across
 * the machines this has run on, so every size below is derived from the
 * framebuffer dimensions at startup. An earlier version hardcoded sizes for a
 * large screen and simply ran off the edge of a small one.
 */

static int SCR_W, SCR_H;
static int SCALE;           /* chrome font scale: titles, taskbar, labels */
static int TERM_SCALE;      /* terminal font scale, deliberately smaller */
static int CELL_W, CELL_H;      /* terminal cell size, from TERM_SCALE */
static int CHROME_W, CHROME_H;  /* chrome cell size, from SCALE */
static int TASKBAR_H;
static int TITLE_H;

#define BORDER 2
#define MAX_WINDOWS 2

/* Upper bounds on the terminal grid; the part actually used is computed from
 * the window size. */
#define MAX_COLS 160
#define MAX_ROWS 80

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

static int  dragging = -1;
static int  drag_dx, drag_dy;
static bool start_menu_open;

static uint32_t col_desktop, col_face, col_edge, col_shade;
static uint32_t col_title_on, col_title_off, col_term_bg, col_term_fg;
static uint32_t col_bar, col_btn, col_btn_on, col_white, col_black, col_accent;
static uint32_t col_close;

/* ---- terminal ------------------------------------------------------------ */

static char term_cells[MAX_ROWS][MAX_COLS];
static int  term_cols, term_rows;
static int  term_cx, term_cy;

/* Scrollback.
 *
 * Lines pushed off the top were previously discarded, which made anything
 * longer than the window -- the self-test, a fault dump, a long help listing
 * -- unreadable the moment it had scrolled. They are kept in a ring now and
 * PgUp pages back through them.
 *
 * The view offset is a display concern only: it never changes what has been
 * written, and any new output snaps back to the bottom so live output is
 * never silently hidden behind a scrolled view.
 */
#define TERM_HISTORY 400

static char term_history[TERM_HISTORY][MAX_COLS];
static int  term_hist_count;
static int  term_hist_next;
static int  term_view_offset;

static void term_clear(void)
{
    memset(term_cells, ' ', sizeof(term_cells));
    term_cx = term_cy = 0;
}

static void term_scroll(void)
{
    /* Keep the line about to be lost. */
    memcpy(term_history[term_hist_next], term_cells[0], (uint32_t)term_cols);
    term_hist_next = (term_hist_next + 1) % TERM_HISTORY;
    if (term_hist_count < TERM_HISTORY)
        term_hist_count++;

    for (int y = 1; y < term_rows; y++)
        memcpy(term_cells[y - 1], term_cells[y], (uint32_t)term_cols);

    memset(term_cells[term_rows - 1], ' ', (uint32_t)term_cols);
    term_cy = term_rows - 1;
}

/* Fetch a displayed row, taking the scroll offset into account. Rows above
 * the live grid come from the ring. */
static const char *term_row(int row)
{
    int from_history = term_view_offset - row;

    if (from_history > 0) {
        if (from_history > term_hist_count)
            return 0;

        int slot = (term_hist_next - from_history + TERM_HISTORY * 2)
                 % TERM_HISTORY;
        return term_history[slot];
    }

    return term_cells[row - term_view_offset];
}

static void term_scroll_view(int lines)
{
    term_view_offset += lines;

    if (term_view_offset > term_hist_count)
        term_view_offset = term_hist_count;
    if (term_view_offset < 0)
        term_view_offset = 0;
}

/* Set while the graphical shell is running, so terminal output can be shown
 * as it is produced. */
static bool gui_active;
static void gui_flush_terminal(void);

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

    if (term_cx >= term_cols) { term_cx = 0; term_cy++; }
    if (term_cy >= term_rows) term_scroll();

    /* Repaint on line boundaries while a command is running.
     *
     * Commands are dispatched from inside the render loop, so nothing is
     * redrawn until they return. A command that takes half a minute -- the
     * guided demo, or the self-test -- therefore produced no output at all
     * until it finished, which is indistinguishable from the system having
     * hung. Flushing here shows the work as it happens. */
    if (gui_active && c == '\n')
        gui_flush_terminal();
}

/* ---- drawing ------------------------------------------------------------- */

static bool in_rect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Clamp a window so its title bar always stays reachable. A window dragged
 * fully off screen would otherwise be impossible to get back. */
static void clamp_window(window_t *win)
{
    int min_visible = 60;

    if (win->x > SCR_W - min_visible) win->x = SCR_W - min_visible;
    if (win->y > SCR_H - TASKBAR_H - TITLE_H) win->y = SCR_H - TASKBAR_H - TITLE_H;
    if (win->x + win->w < min_visible) win->x = min_visible - win->w;
    if (win->y < 0) win->y = 0;
}

#define WIN_RADIUS 8

static void draw_window_frame(const window_t *win, bool active)
{
    /* The focused window gets a deeper shadow. Depth is how a desktop shows
     * which window is in front without needing a colour change. */
    fb_shadow(win->x, win->y, win->w, win->h, WIN_RADIUS, active ? 9 : 5);

    fb_fill_round_rect(win->x, win->y, win->w, win->h, WIN_RADIUS, col_face);

    /* Title bar. Its bottom corners are square so it meets the window body
     * flush, which means drawing it as a rounded rect and then squaring off
     * the lower half. */
    fb_fill_round_rect(win->x, win->y, win->w, TITLE_H, WIN_RADIUS,
                       active ? col_title_on : col_title_off);
    fb_fill_rect(win->x, win->y + TITLE_H - WIN_RADIUS, win->w, WIN_RADIUS,
                 active ? col_title_on : col_title_off);

    /* A single lighter line along the top edge. Real interfaces suggest a
     * light source rather than drawing a border, and one row is enough. */
    fb_blend_rect(win->x + WIN_RADIUS, win->y, win->w - 2 * WIN_RADIUS, 1,
                  col_white, active ? 70 : 40);

    fb_text_aa(win->x + 14, win->y + (TITLE_H - fb_font_height(true)) / 2,
               win->title, col_white, true);

    /* Close button: a circle that only shows its colour on the focused
     * window, which is how it reads as inactive rather than disabled. */
    int bs = TITLE_H - 14;
    int bx = win->x + win->w - bs - 12;
    int by = win->y + 7;

    fb_fill_round_rect(bx, by, bs, bs, bs / 2,
                       active ? col_close : col_title_off);

    for (int i = 4; i < bs - 4; i++) {
        fb_blend_rect(bx + i, by + i, 2, 2, col_white, 220);
        fb_blend_rect(bx + bs - 1 - i, by + i, 2, 2, col_white, 220);
    }
}

static void close_box_rect(const window_t *win, int *bx, int *by, int *bs)
{
    *bs = TITLE_H - 14;
    *bx = win->x + win->w - *bs - 12;
    *by = win->y + 7;
}

static void draw_terminal(const window_t *win, bool active)
{
    int tx = win->x + BORDER + 4;
    int ty = win->y + TITLE_H + BORDER + 4;

    fb_fill_round_rect(win->x + 6, win->y + TITLE_H + 2,
                       win->w - 12, win->h - TITLE_H - 8, 5, col_term_bg);

    for (int row = 0; row < term_rows; row++) {
        const char *line = term_row(row);
        if (!line)
            continue;

        for (int col = 0; col < term_cols; col++) {
            char c = line[col];
            if (c != ' ')
                fb_char_aa(tx + col * CELL_W, ty + row * CELL_H,
                           c, col_term_fg, false);
        }
    }

    /* A marker while scrolled back, so it is obvious the view is not live. */
    if (term_view_offset > 0)
        fb_blend_rect(win->x + win->w - 10, win->y + TITLE_H + 4,
                      4, win->h - TITLE_H - 12, col_accent, 120);

    if (active && term_view_offset == 0)
        fb_fill_rect(tx + term_cx * CELL_W,
                     ty + term_cy * CELL_H + CELL_H - 2,
                     CELL_W, 2, col_term_fg);
}

static void draw_number(int x, int y, uint32_t v, uint32_t colour)
{
    char digits[12], out[12];
    int  p = 0, q = 0;

    if (v == 0)
        digits[p++] = '0';
    while (v > 0) { digits[p++] = (char)('0' + (v % 10)); v /= 10; }
    while (p > 0) out[q++] = digits[--p];
    out[q] = '\0';

    fb_text_aa(x, y, out, colour, true);
}

static void draw_stats(const window_t *win)
{
    heap_stats_t heap;
    heap_get_stats(&heap);

    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;

    static const char *labels[5] = {
        "tasks", "uptime s", "heap KB", "switches", "mouse"
    };
    uint32_t values[5] = {
        (uint32_t)task_count(), secs, heap.used_bytes / 1024,
        task_switch_count(), mouse_packet_count()
    };

    int tx = win->x + 10;
    int ty = win->y + TITLE_H + 10;
    int step = CHROME_H + 5;

    for (int i = 0; i < 5; i++) {
        fb_text_aa(tx, ty + i * step, labels[i], col_black, true);
        draw_number(tx + 10 * CHROME_W, ty + i * step, values[i], col_title_on);
    }
}

static void draw_start_menu(void)
{
    int mw = 18 * CHROME_W;
    int mh = 3 * (CHROME_H + 10) + 12;
    int mx = 4;
    int my = SCR_H - TASKBAR_H - mh;

    fb_shadow(mx, my, mw, mh, 8, 8);
    fb_fill_round_rect(mx, my, mw, mh, 8, col_face);

    static const char *items[3] = { "Terminal", "System", "Exit to console" };

    for (int i = 0; i < 3; i++) {
        int iy = my + 6 + i * (CHROME_H + 10);
        fb_fill_round_rect(mx + 5, iy, mw - 10, CHROME_H + 6, 4, col_face);
        fb_text_aa(mx + 12, iy + 3, items[i], col_black, true);
    }
}

static void draw_taskbar(void)
{
    int bar_y = SCR_H - TASKBAR_H;

    fb_fill_rect(0, bar_y, SCR_W, TASKBAR_H, col_bar);

    /* A hairline of light along the top rather than a coloured band. */
    fb_blend_rect(0, bar_y, SCR_W, 1, col_white, 45);

    /* Start button. */
    int sw = 7 * CHROME_W;
    fb_fill_round_rect(8, bar_y + 6, sw, TASKBAR_H - 12, 6,
                       start_menu_open ? col_btn_on : col_btn);
    fb_text_aa(16, bar_y + (TASKBAR_H - fb_font_height(true)) / 2, "Start", col_white, true);

    /* One button per window. Width is divided from the space that is actually
     * left, rather than assumed, so the labels never run into the clock. */
    int first = sw + 16;
    int clock_w = 9 * CHROME_W;
    int avail = SCR_W - first - clock_w - 16;
    int bw = avail / MAX_WINDOWS;

    if (bw > 18 * CHROME_W) bw = 20 * CELL_W;

    for (int i = 0; i < window_count && bw > 5 * CHROME_W; i++) {
        int bx = first + i * (bw + 6);

        fb_fill_round_rect(bx, bar_y + 6, bw - 8, TASKBAR_H - 12, 6,
                           windows[i].visible ? col_btn_on : col_btn);
        fb_text_aa(bx + 10, bar_y + (TASKBAR_H - fb_font_height(true)) / 2,
                   windows[i].title, col_white, true);
    }

    /* Uptime where a clock would be. There is no real-time clock driver, so
     * showing a wall clock would mean inventing one. */
    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;
    int cx = SCR_W - clock_w - 6;

    fb_text_aa(cx, bar_y + (TASKBAR_H - fb_font_height(true)) / 2, "up", col_white, true);
    draw_number(cx + 3 * CHROME_W, bar_y + (TASKBAR_H - 8 * SCALE) / 2,
                secs, col_white);
}

/* ---- cursor -------------------------------------------------------------
 *
 * The cursor is composited separately from the rest of the interface, and
 * this is the single most important thing in the render loop.
 *
 * Drawing it as part of the scene means the scene has to be repainted every
 * time the pointer moves a pixel: at 1920x1080 that is a full 8.3 MB redraw
 * and copy to answer a mouse interrupt, which is what made the pointer feel
 * as though it were dragging. Instead the pixels underneath are saved before
 * the cursor is drawn and put back before it moves, so a mouse movement
 * touches two small rectangles and nothing else.
 */

#define CURSOR_W 16
#define CURSOR_H 24

static uint32_t cursor_backing[CURSOR_W * 4 * CURSOR_H * 4];
static int      cursor_saved_x, cursor_saved_y;
static int      cursor_saved_w, cursor_saved_h;
static bool     cursor_visible;

static void cursor_shape(int mx, int my, int s)
{
    for (int i = 0; i < 14; i++) {
        fb_fill_rect(mx, my + i * s, s, s, col_black);
        if (i > 0 && i < 11) fb_fill_rect(mx + s, my + i * s, s, s, col_white);
        if (i > 1 && i < 9)  fb_fill_rect(mx + 2 * s, my + i * s, s, s, col_white);
    }

    for (int i = 0; i < 7; i++)
        fb_fill_rect(mx + (3 + i) * s, my + (4 + i) * s, s, s,
                     i < 4 ? col_white : col_black);
}

/* Put back whatever the cursor was covering. */
static void cursor_erase(void)
{
    if (!cursor_visible)
        return;

    fb_write_rect(cursor_saved_x, cursor_saved_y,
                  cursor_saved_w, cursor_saved_h, cursor_backing);

    cursor_visible = false;
}

/* Save what is about to be covered, then draw. */
static void cursor_draw(void)
{
    int s = SCALE;
    int w = CURSOR_W * s;
    int h = CURSOR_H * s;

    cursor_saved_x = mouse_x();
    cursor_saved_y = mouse_y();
    cursor_saved_w = w;
    cursor_saved_h = h;

    fb_read_rect(cursor_saved_x, cursor_saved_y, w, h, cursor_backing);
    cursor_shape(cursor_saved_x, cursor_saved_y, s);

    cursor_visible = true;
}

/* Redraw just the terminal window and push it, without touching the rest of
 * the scene. Used for live output while a command is still running. */
static void gui_flush_terminal(void)
{
    for (int i = 0; i < window_count; i++) {
        if (windows[i].kind != WIN_TERMINAL || !windows[i].visible)
            continue;

        cursor_erase();
        draw_terminal(&windows[i], i == focused);
        cursor_draw();
        fb_present();
        return;
    }
}

/* ---- input --------------------------------------------------------------- */

static void raise_window(int index)
{
    if (index < 0 || index == window_count - 1)
        return;

    window_t tmp = windows[index];
    for (int i = index; i < window_count - 1; i++)
        windows[i] = windows[i + 1];
    windows[window_count - 1] = tmp;
}

static void show_window(window_kind_t kind)
{
    for (int i = 0; i < window_count; i++) {
        if (windows[i].kind != kind)
            continue;

        windows[i].visible = true;
        raise_window(i);
        focused = window_count - 1;
        return;
    }
}

/* Returns false if the GUI should exit. */
static bool handle_click(int mx, int my)
{
    int bar_y = SCR_H - TASKBAR_H;

    if (start_menu_open) {
        int mw = 18 * CHROME_W;
        int mh = 3 * (CHROME_H + 10) + 12;
        int sx = 4;
        int sy = SCR_H - TASKBAR_H - mh;

        if (in_rect(mx, my, sx, sy, mw, mh)) {
            int item = (my - sy - 6) / (CELL_H + 10);
            start_menu_open = false;

            if (item == 0) show_window(WIN_TERMINAL);
            else if (item == 1) show_window(WIN_STATS);
            else if (item == 2) return false;

            return true;
        }

        start_menu_open = false;   /* clicked away: dismiss */
    }

    if (my >= bar_y) {
        int sw = 7 * CHROME_W;

        if (in_rect(mx, my, 6, bar_y + 5, sw, TASKBAR_H - 10)) {
            start_menu_open = !start_menu_open;
            return true;
        }

        int first = sw + 16;
        int clock_w = 9 * CHROME_W;
        int avail = SCR_W - first - clock_w - 16;
        int bw = avail / MAX_WINDOWS;
        if (bw > 18 * CHROME_W) bw = 20 * CELL_W;

        for (int i = 0; i < window_count; i++) {
            int bx = first + i * (bw + 6);
            if (in_rect(mx, my, bx, bar_y + 5, bw - 6, TASKBAR_H - 10)) {
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

        int bx, by, bs;
        close_box_rect(&windows[i], &bx, &by, &bs);

        if (in_rect(mx, my, bx, by, bs, bs)) {
            windows[i].visible = false;
            return true;
        }

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

    SCR_W = (int)fb_width();
    SCR_H = (int)fb_height();

    /* Small screens get an unscaled font, or almost nothing fits. */
    SCALE = (SCR_W >= 1000) ? 2 : 1;

    /* The terminal uses a smaller font than the window chrome.
     *
     * Every command in the shell formats its output for an 80-column screen —
     * the task table, the heap report, the self-test. At the chrome scale a
     * window occupying two thirds of a 1024-wide display is only 41 columns,
     * so those tables wrapped mid-row and became unreadable. Halving the font
     * for terminal content buys back the columns the output was written for. */
    TERM_SCALE = (SCR_W >= 1600) ? 2 : 1;

    /* Metrics come from the font now. The terminal grid is the monospaced
     * advance; chrome spacing is measured from a representative character
     * rather than assumed, because the interface face is proportional. */
    CELL_W    = fb_mono_advance();
    CELL_H    = fb_font_height(false);
    CHROME_W  = fb_text_width("M", true);
    CHROME_H  = fb_font_height(true);
    TASKBAR_H = CHROME_H + 20;
    TITLE_H   = CHROME_H + 12;

    /* Muted and low-contrast. Saturated primaries are most of what makes an
     * interface look like a toy; desktops sit in a narrow band of desaturated
     * greys and blues and put contrast only where it carries meaning. */
    col_desktop   = fb_rgb(26,  34,  48);
    col_face      = fb_rgb(243, 244, 246);
    col_edge      = fb_rgb(255, 255, 255);
    col_shade     = fb_rgb(10,  14,  22);
    col_title_on  = fb_rgb(46,  62,  92);
    col_title_off = fb_rgb(120, 128, 142);
    col_term_bg   = fb_rgb(22,  26,  34);
    col_term_fg   = fb_rgb(178, 220, 190);
    col_bar       = fb_rgb(30,  36,  48);
    col_btn       = fb_rgb(52,  60,  76);
    col_btn_on    = fb_rgb(70,  110, 170);
    col_accent    = fb_rgb(138, 170, 214);
    col_close     = fb_rgb(198, 88,  84);
    col_white     = fb_rgb(255, 255, 255);
    col_black     = fb_rgb(0,   0,   0);

    /* The stats window is sized first, from the longest label it has to show,
     * and the terminal then takes the width that is left. Sizing the terminal
     * first as a fraction of the screen left the two overlapping on startup:
     * correct behaviour from the window manager, but a poor first impression. */
    int sw = 17 * CHROME_W;
    int sh = 5 * (CHROME_H + 5) + TITLE_H + 24;

    int tw = SCR_W - sw - 34;
    int th = SCR_H - TASKBAR_H - 60;

    if (tw < 40 * CELL_W)
        tw = SCR_W - 20;        /* too narrow to sit alongside; use full width */

    term_cols = (tw - 2 * BORDER - 8) / CELL_W;
    term_rows = (th - TITLE_H - 2 * BORDER - 8) / CELL_H;

    if (term_cols > MAX_COLS) term_cols = MAX_COLS;
    if (term_rows > MAX_ROWS) term_rows = MAX_ROWS;
    if (term_cols < 20) term_cols = 20;
    if (term_rows < 6)  term_rows = 6;

    tw = term_cols * CELL_W + 2 * BORDER + 8;
    th = term_rows * CELL_H + TITLE_H + 2 * BORDER + 8;

    term_clear();

    window_count = 2;
    windows[0] = (window_t){ SCR_W - sw - 12, 30, sw, sh,
                             "System", WIN_STATS, true };
    windows[1] = (window_t){ 10, 40, tw, th,
                             "Terminal", WIN_TERMINAL, true };
    focused = 1;

    for (int i = 0; i < window_count; i++)
        clamp_window(&windows[i]);

    start_menu_open = false;
    dragging = -1;

    mouse_set_bounds(SCR_W, SCR_H);
    gui_active = true;
    console_set_sink(term_putc);

    kprintf("MyOS graphical mode - %dx%d\n", SCR_W, SCR_H);
    kprintf("Start menu opens windows. Drag by the title bar.\n");
    kprintf("try: help, tasks, meminfo\n\n> ");

    /* Two different notions of "needs work". scene_dirty means the windows or
     * their contents changed and the interface has to be repainted. dirty
     * means only that something happened at all -- most often the pointer
     * moving, which needs the cursor recomposited and nothing else. */
    bool     scene_dirty = true;
    bool     dirty = true;
    int      last_mx = mouse_x(), last_my = mouse_y();
    uint32_t last_frame = pit_ticks();

    for (;;) {
        while (kbd_available()) {
            char c = kbd_poll();

            if (c == 27) {
                gui_active = false;
                console_set_sink(0);
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
            } else if (c == KEY_PGUP) {
                term_scroll_view(term_rows / 2);
            } else if (c == KEY_PGDN) {
                term_scroll_view(-(term_rows / 2));
            } else if (c >= 32 && c < 127 && len < (int)sizeof(line) - 1) {
                line[len++] = c;
                kputc(c);
            }

            dirty = true;
            scene_dirty = true;   /* typed text is part of the scene */
        }

        if (mouse_take_click(MOUSE_LEFT)) {
            if (!handle_click(mouse_x(), mouse_y())) {
                gui_active = false;
                console_set_sink(0);
                fbcon_clear();
                return;
            }
            dirty = true;
            scene_dirty = true;
        }

        if (dragging >= 0) {
            if (mouse_buttons() & MOUSE_LEFT) {
                windows[dragging].x = mouse_x() - drag_dx;
                windows[dragging].y = mouse_y() - drag_dy;
                clamp_window(&windows[dragging]);
                dirty = true;
                scene_dirty = true;
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

        last_frame = pit_ticks();

        /* The cursor sits on top of the scene, so it has to come off before
         * anything underneath is touched, and go back on afterwards. */
        cursor_erase();

        if (scene_dirty) {
            scene_dirty = false;

            fb_fill_rect(0, 0, SCR_W, SCR_H - TASKBAR_H, col_desktop);
            fb_text_aa(14, 10, "MyOS", col_white, true);
            fb_text_aa(14 + fb_text_width("MyOS   ", true), 10,
                       "an operating system, running on the hardware",
                       col_accent, true);

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

            if (start_menu_open)
                draw_start_menu();
        }

        /* One present per frame, covering the old cursor position and the new
         * one together.
         *
         * Presenting between the erase and the redraw seemed tidier -- it
         * keeps the dirty box small when the pointer jumps a long way -- but
         * it puts a frame on screen with the cursor missing, and at this
         * frame rate that reads as a flicker. Doing it in one pass costs a
         * larger rectangle occasionally and never shows a half-drawn state. */
        cursor_draw();
        fb_present();

        task_yield();
    }
}
