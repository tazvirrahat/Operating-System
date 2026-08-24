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
#include "rtc.h"
#include "wallpaper.h"
#include "fs.h"
#include "demos.h"
#include "pci.h"
#include "svga.h"
#include "paging.h"

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
#define MAX_WINDOWS 6

/* Upper bounds on the terminal grid; the part actually used is computed from
 * the window size. */
#define MAX_COLS 160
#define MAX_ROWS 80

typedef enum {
    WIN_TERMINAL,
    WIN_STATS,
    WIN_FILES,
    WIN_DEMOS,          /* Kernel Lab: real kernel experiments, not a simulation */
    WIN_TASKS,
    WIN_NOTEPAD
} window_kind_t;

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

/* A dragged window's contents do not change -- only where they are.
 *
 * Repainting the desktop for every mouse packet was costing a full wallpaper
 * (two million palette lookups), a re-render of every window including all of
 * the anti-aliased text in them, and an 8 MB present, at the rate the mouse
 * reports. Rasterising the window once and moving those pixels is what a
 * compositor does, and it turns the per-frame cost into two rectangle copies.
 */
static uint32_t *drag_cache;
static int       drag_cache_w, drag_cache_h;
static bool      drag_capture;     /* grab the pixels on the next repaint */
static int       drag_prev_x, drag_prev_y;
static bool      drag_moved;
static uint32_t  drag_last_frame;

/* Hand window moves to the adapter's blit engine.
 *
 * Off by default, because it was measured and it lost. Over a ten-second
 * drag under QEMU's emulated VMware adapter the idle task got 33% of the
 * ticks with this off and 27-30% with it on, repeatably.
 *
 * The reason is that neither half of the trade pays there. RECT_COPY is
 * executed by the host CPU, so no real blit engine is saved; and the
 * ordering it forces costs more than the copy did. The FIFO runs
 * asynchronously, so before the CPU may write the uncovered strips into
 * video memory it has to wait for the adapter to finish reading the
 * rectangle it is moving -- and svga_sync polls a register over an I/O
 * port, which is a VM exit per read, every frame.
 *
 * On a VMware Workstation guest the same command is backed by the host's
 * GPU, where the blit really is free and the trade may well go the other
 * way. Flip this to true and compare the idle column in `tasks` across a
 * drag; that is the measurement, and it should be made on the machine the
 * answer is wanted for rather than assumed from this one.
 */
static const bool accel_moves = false;
static bool start_menu_open;

static uint32_t col_desktop, col_face, col_edge, col_shade;
static uint32_t col_title_on, col_title_off, col_term_bg, col_term_fg;
static uint32_t col_bar, col_btn, col_btn_on, col_white, col_black, col_accent;
static uint32_t col_close;
static uint32_t col_search, col_search_fg;
static uint32_t col_menu, col_menu_fg;
static uint32_t col_head, col_list_bg, col_list_sel, col_list_sel_off;
static uint32_t col_pane, col_paper, col_folder, col_folder_tab;
static uint32_t col_ok;

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
static void gui_flush_demos(void);

/* When the terminal was last painted, so a command that produces no
 * newlines still shows progress. */
static uint32_t term_last_flush;

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

    /* Repaint while a command is running.
     *
     * Commands are dispatched from inside the render loop, so nothing is
     * redrawn until they return. A command that takes half a minute -- the
     * guided demo, or the self-test -- therefore produced no output at all
     * until it finished, which is indistinguishable from the system having
     * hung. Flushing here shows the work as it happens.
     *
     * Line boundaries alone were not enough. The multitasking demo prints one
     * character per task with no newline until every task has finished, so
     * the very demonstration whose point is watching three tasks interleave
     * was the one that arrived all at once at the end. The elapsed-time
     * condition covers that without flushing on every character, which at
     * this window size would cost more than the output is worth. */
    if (gui_active &&
        (c == '\n' || pit_ticks() - term_last_flush >= 10))
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

/* Returns the length, so a caller that needs to right-align can measure the
 * string before drawing it. */
static int u32_to_str(uint32_t v, char *out)
{
    char digits[12];
    int  p = 0, q = 0;

    if (v == 0)
        digits[p++] = '0';
    while (v > 0) { digits[p++] = (char)('0' + (v % 10)); v /= 10; }
    while (p > 0) out[q++] = digits[--p];
    out[q] = '\0';

    return q;
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
        char v[12];
        u32_to_str(values[i], v);
        fb_text_aa(win->x + win->w - 14 - fb_text_width(v, true),
                   ty + i * step, v, col_title_on, true);
    }
}

/* The Start menu's geometry is needed both to draw it and to work out what a
 * click landed on. Two copies of these expressions is how a menu item ends up
 * one row off from the thing it runs, so there is only one. */
#define START_ITEMS 8

static const char *start_items[START_ITEMS] = {
    "Terminal", "File Explorer", "Task Manager", "Notepad",
    "Kernel Lab", "System", "Change wallpaper", "Exit to console"
};

static void start_menu_box(int *x, int *y, int *w, int *h, int *row)
{
    int rh = CHROME_H + 16;

    *row = rh;
    *w   = 21 * CHROME_W;
    *h   = START_ITEMS * rh + 16;
    *x   = 0;                               /* flush left, as on Windows */
    *y   = SCR_H - TASKBAR_H - *h;
}

static void draw_start_menu(void)
{
    int mx, my, mw, mh, rh;
    start_menu_box(&mx, &my, &mw, &mh, &rh);

    fb_shadow(mx, my, mw, mh, 10, 10);
    fb_fill_rect(mx, my, mw, mh, col_menu);

    /* A hairline along the top and right edges. Without it the panel merges
     * into a dark wallpaper and stops reading as a surface. */
    fb_blend_rect(mx, my, mw, 1, col_white, 26);
    fb_blend_rect(mx + mw - 1, my, 1, mh, col_white, 26);

    for (int i = 0; i < START_ITEMS; i++) {
        int iy = my + 8 + i * rh;

        fb_text_aa(mx + 16, iy + (rh - CHROME_H) / 2,
                   start_items[i], col_menu_fg, true);
    }
}

/* The taskbar follows the Windows layout: a square Start button hard against
 * the left edge, a search field beside it, then one flat button per window
 * with an accent underline on the active one, and the clock at the far right.
 *
 * Nothing here is rounded. The rounding used on windows would be wrong on a
 * bar that meets three screen edges -- the corners have nowhere to sit. */
/* ---- file explorer -------------------------------------------------------
 *
 * The filesystem existed before this window did, and the only way to reach it
 * was to type `ls` and `cat`. This is the part that makes it visible: the same
 * namespace, listed and selectable, with the contents of whatever is selected
 * shown beside it.
 *
 * It keeps no copy of anything. Every frame walks the filesystem afresh, so a
 * file written from the terminal appears here without the terminal having to
 * know this window exists. A cached listing would need invalidating from every
 * place that can create or delete a file, and would be stale the first time
 * somebody forgot.
 */

static int files_sel;       /* a row in the listing, not a slot in the table */
static int files_scroll;
static uint32_t files_click_tick;
static int      files_click_row = -1;
static bool     files_open_notepad;

static void show_window(window_kind_t kind);
static void notepad_open_named(const char *name);

/* The listing skips empty slots, so row numbers and slot numbers are not the
 * same thing. Everything below counts in rows and resolves to a file here. */
static const fs_file_t *file_by_row(int row)
{
    int seen = 0;

    if (row < 0)
        return 0;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        const fs_file_t *f = fs_at(i);
        if (!f)
            continue;
        if (seen == row)
            return f;
        seen++;
    }

    return 0;
}

typedef struct {
    int cx, cy, cw, ch;         /* content box inside the window frame */
    int strip_h;                /* the path bar across the top */
    int head_y;                 /* the column headings */
    int list_x, list_y, list_w;
    int row_h, rows;
    int name_x, name_w;         /* columns, measured rather than guessed */
    int size_right, when_x;
    int pane_x, pane_y, pane_w, pane_h;
    int status_y;
} files_layout_t;

/* Drawing and hit-testing both come here rather than each computing the same
 * offsets. A file manager whose rows are drawn in one place and clicked in
 * another is how you select the file above the one you pointed at. */
static void files_layout(const window_t *win, files_layout_t *L)
{
    L->cx = win->x + BORDER + 8;
    L->cy = win->y + TITLE_H + BORDER + 8;
    L->cw = win->w - 2 * (BORDER + 8);
    L->ch = win->h - TITLE_H - 2 * BORDER - 16;

    L->strip_h = CHROME_H + 14;
    L->head_y  = L->cy + L->strip_h + 8;

    L->row_h  = CHROME_H + 8;
    L->list_x = L->cx;
    L->list_y = L->head_y + CHROME_H + 8;
    L->list_w = (L->cw * 60) / 100;

    /* The size and date columns are given exactly the width of the widest
     * value they can hold; the name gets the remainder. Fixed pixel widths
     * were the first version, and the name ran into the size the moment the
     * font or the window changed. */
    int size_w = fb_text_width("999999", true) + 10;
    int when_w = fb_text_width("00:00 00 Aug 0000", true) + 10;

    L->when_x     = L->list_x + L->list_w - when_w;
    L->size_right = L->when_x - 12;
    L->name_x     = L->list_x + 34;
    L->name_w     = L->size_right - size_w - L->name_x;

    if (L->name_w < 40)
        L->name_w = 40;

    L->pane_x = L->cx + L->list_w + 10;
    L->pane_y = L->head_y;
    L->pane_w = L->cw - L->list_w - 10;

    L->status_y = L->cy + L->ch - CHROME_H - 2;


    int body = L->status_y - L->list_y - 8;

    L->rows = body / L->row_h;
    if (L->rows < 1)
        L->rows = 1;

    L->pane_h = L->status_y - L->pane_y - 8;
    if (L->pane_h < L->row_h)
        L->pane_h = L->row_h;
}

/* Copies as much of a name as fits, ending in ".." when it did not. A name
 * that overruns its column is worse than a shortened one: it draws over the
 * next column and both become unreadable. */
static void fit_text(const char *src, char *dst, int cap, int max_w)
{
    int n = 0;

    dst[0] = '\0';

    while (src[n] && n < cap - 1) {
        dst[n]     = src[n];
        dst[n + 1] = '\0';

        if (fb_text_width(dst, true) > max_w) {
            if (n >= 2) {
                dst[n - 1] = '.';
                dst[n]     = '.';
                dst[n + 1] = '\0';
            } else {
                dst[0] = '\0';
            }
            return;
        }

        n++;
    }
}

/* A folder: a body with a tab along the top left, which is the shape every
 * desktop uses and is recognisable at this size without any detail. */
static void draw_folder(int x, int y, int w, int h, uint32_t body, uint32_t tab)
{
    int tab_w = (w * 45) / 100;
    int tab_h = h / 4;

    if (tab_h < 2) tab_h = 2;

    fb_fill_rect(x, y + tab_h, w, h - tab_h, body);
    fb_fill_rect(x, y, tab_w, tab_h, tab);
}

/* A sheet with the top right corner turned down. */
static void draw_page(int x, int y, int w, int h, uint32_t paper, uint32_t ink)
{
    int fold = w / 3;

    fb_fill_rect(x, y, w, h, paper);

    /* An outline. Without it a near-white sheet on a near-white listing is
     * only visible where the folded corner is, which reads as a smudge. */
    fb_blend_rect(x, y, w, 1, ink, 130);
    fb_blend_rect(x, y + h - 1, w, 1, ink, 130);
    fb_blend_rect(x, y, 1, h, ink, 130);
    fb_blend_rect(x + w - 1, y, 1, h, ink, 130);

    fb_fill_rect(x + w - fold, y, fold, fold, ink);

    /* Two ruled lines, enough to read as text without being text. */
    for (int i = 1; i <= 2; i++) {
        int ly = y + h / 3 + (i - 1) * (h / 4);
        if (ly < y + h - 2)
            fb_blend_rect(x + 2, ly, w - 4, 1, ink, 150);
    }
}

/* Right-aligned, because a column of sizes that is not right-aligned cannot
 * be compared at a glance, which is most of what a size column is for. */
static void draw_number_right(int right, int y, uint32_t v, uint32_t colour)
{
    char out[12];
    int  n = u32_to_str(v, out);

    (void)n;
    fb_text_aa(right - fb_text_width(out, true), y, out, colour, true);
}

static void draw_files(const window_t *win, bool active)
{
    files_layout_t L;
    files_layout(win, &L);

    int count = fs_file_count();

    if (files_sel >= count) files_sel = count - 1;
    if (files_sel < 0)      files_sel = 0;

    /* Keep the selection on screen. Clicking cannot move it out of view, but
     * deleting a file from the terminal can. */
    if (files_sel < files_scroll)
        files_scroll = files_sel;
    if (files_sel >= files_scroll + L.rows)
        files_scroll = files_sel - L.rows + 1;
    if (files_scroll > count - L.rows) files_scroll = count - L.rows;
    if (files_scroll < 0)              files_scroll = 0;

    /* Path bar. There is one directory and it has no name, so this says where
     * you are rather than pretending to be a path you can edit. */
    fb_fill_rect(L.cx, L.cy, L.cw, L.strip_h, col_head);
    draw_folder(L.cx + 10, L.cy + L.strip_h / 2 - 7, 18, 14,
                col_folder, col_folder_tab);
    fb_text_aa(L.cx + 36, L.cy + (L.strip_h - CHROME_H) / 2,
               "This PC  >  Files", col_black, true);

    /* Column headings. */
    int size_right = L.size_right;
    int when_x     = L.when_x;

    fb_text_aa(L.name_x, L.head_y, "Name", col_title_off, true);
    fb_text_aa(size_right - fb_text_width("Size", true), L.head_y,
               "Size", col_title_off, true);
    fb_text_aa(when_x, L.head_y, "Modified", col_title_off, true);

    fb_blend_rect(L.list_x, L.list_y - 5, L.list_w, 1, col_black, 50);

    /* The list. */
    fb_fill_rect(L.list_x, L.list_y, L.list_w, L.rows * L.row_h, col_list_bg);

    for (int r = 0; r < L.rows; r++) {
        const fs_file_t *f = file_by_row(files_scroll + r);
        if (!f)
            break;

        int y = L.list_y + r * L.row_h;
        bool sel = (files_scroll + r) == files_sel;
        uint32_t fg = col_black;

        if (sel) {
            fb_fill_rect(L.list_x, y, L.list_w, L.row_h,
                         active ? col_list_sel : col_list_sel_off);
            fg = active ? col_white : col_black;
        }

        draw_page(L.list_x + 8, y + (L.row_h - 14) / 2, 12, 14,
                  sel && active ? col_white : col_paper, col_title_off);

        char shown[FS_NAME_MAX + 2];
        fit_text(f->name, shown, (int)sizeof(shown), L.name_w);

        fb_text_aa(L.name_x, y + (L.row_h - CHROME_H) / 2, shown, fg, true);
        draw_number_right(size_right, y + (L.row_h - CHROME_H) / 2, f->size, fg);

        /* "20:24 18 Aug 2026", assembled by hand because there is no
         * snprintf here and the console formatter writes to the console. */
        char when[24];
        int  n = 0;

        when[n++] = (char)('0' + f->hour / 10);
        when[n++] = (char)('0' + f->hour % 10);
        when[n++] = ':';
        when[n++] = (char)('0' + f->minute / 10);
        when[n++] = (char)('0' + f->minute % 10);
        when[n++] = ' ';
        when[n++] = (char)('0' + f->day / 10);
        when[n++] = (char)('0' + f->day % 10);
        when[n++] = ' ';

        const char *mon = rtc_month_name(f->month);
        for (int i = 0; mon[i] && i < 3; i++)
            when[n++] = mon[i];

        when[n++] = ' ';
        when[n++] = (char)('0' + (f->year / 1000) % 10);
        when[n++] = (char)('0' + (f->year / 100) % 10);
        when[n++] = (char)('0' + (f->year / 10) % 10);
        when[n++] = (char)('0' + f->year % 10);
        when[n]   = '\0';

        fb_text_aa(when_x, y + (L.row_h - CHROME_H) / 2, when, fg, true);
    }

    if (count == 0)
        fb_text_aa(L.name_x, L.list_y + 6,
                   "This folder is empty.", col_title_off, true);

    /* Preview. The selected file's contents, in the monospaced face, because
     * a preview that reflows is a preview of something else. */
    fb_fill_rect(L.pane_x, L.pane_y, L.pane_w, L.pane_h, col_pane);

    const fs_file_t *sel = file_by_row(files_sel);
    int adv = fb_mono_advance();
    int px  = L.pane_x + 8;
    int py  = L.pane_y + 6;

    if (!sel) {
        fb_text_aa(px, py, "Nothing selected", col_title_off, true);
    } else {
        fb_text_aa(px, py, sel->name, col_title_on, true);
        py += CHROME_H + 6;
        fb_blend_rect(L.pane_x + 6, py - 3, L.pane_w - 12, 1, col_black, 40);

        int cols = (L.pane_w - 16) / (adv ? adv : 8);
        int max_rows = (L.pane_y + L.pane_h - py) / CELL_H;
        int col = 0, row = 0;

        for (uint32_t i = 0; i < sel->size && row < max_rows; i++) {
            char c = (char)sel->data[i];

            if (c == '\n') {
                col = 0;
                row++;
                continue;
            }

            if (c == '\t') {
                col = (col + 4) & ~3;
            } else {
                if (c < 32 || c > 126)
                    c = '.';

                fb_char_aa(px + col * adv, py + row * CELL_H, c,
                           col_term_fg, false);
                col++;
            }

            if (col >= cols) {
                col = 0;
                row++;
            }
        }

        if (sel->size == 0)
            fb_text_aa(px, py, "(empty file)", col_title_off, true);
    }

    /* Status bar, which is where a file manager says how much is here. */
    char status[48];
    int  n = 0;

    n += u32_to_str((uint32_t)count, status + n);
    status[n++] = ' ';
    status[n++] = 'i'; status[n++] = 't'; status[n++] = 'e'; status[n++] = 'm';
    if (count != 1) status[n++] = 's';
    status[n++] = ','; status[n++] = ' ';
    n += u32_to_str(fs_bytes_used(), status + n);
    status[n++] = ' ';
    status[n++] = 'b'; status[n++] = 'y'; status[n++] = 't'; status[n++] = 'e';
    status[n++] = 's';
    status[n]   = '\0';

    fb_text_aa(L.cx + 2, L.status_y, status, col_title_off, true);
}

/* Returns true if the click was inside the listing and changed the selection.
 * The caller still raises and focuses the window either way. */
static bool files_click(const window_t *win, int mx, int my)
{
    files_layout_t L;
    files_layout(win, &L);

    if (!in_rect(mx, my, L.list_x, L.list_y, L.list_w, L.rows * L.row_h))
        return false;

    int row = files_scroll + (my - L.list_y) / L.row_h;

    if (!file_by_row(row))
        return false;

    /* A second click on the same row inside a few ticks is a double-click,
     * and opens the file in Notepad. A cached "last click" is the whole
     * gesture; there is no second pointer button. */
    if (row == files_sel && row == files_click_row
        && pit_ticks() - files_click_tick < 40) {
        const fs_file_t *f = file_by_row(row);
        if (f) {
            notepad_open_named(f->name);
            files_open_notepad = true;
        }
    }

    files_sel = row;
    files_click_row = row;
    files_click_tick = pit_ticks();
    return true;
}

/* ---- task manager --------------------------------------------------------
 *
 * The same list `tasks` prints, drawn as a window and sampled every frame
 * so the numbers move. CPU share is counted from the scheduler's tick
 * ring rather than invented: of the last N timer ticks, how many this
 * task owned. End task is task_kill, which already refuses the kernel
 * task — the GUI *is* that task, so a mis-click must not take down the
 * desktop. */

static int  tm_sel;         /* index in the live list, not a PID */
static int  tm_scroll;
static char tm_msg[64];

typedef struct {
    int cx, cy, cw, ch;
    int btn_x, btn_y, btn_w, btn_h;
    int trace_y, trace_h;
    int head_y;
    int list_x, list_y, list_w, row_h, rows;
    int pid_x, name_x, state_x, cpu_x, mem_x, ticks_x;
    int summary_y, summary_h;
    int status_y;
} tm_layout_t;

static uint32_t task_colour(uint32_t id);
static void     draw_tick_strip(int x, int y, int w, int h);

static void tm_layout(const window_t *win, tm_layout_t *L)
{
    L->cx = win->x + BORDER + 8;
    L->cy = win->y + TITLE_H + BORDER + 8;
    L->cw = win->w - 2 * (BORDER + 8);
    L->ch = win->h - TITLE_H - 2 * (BORDER + 8);

    L->btn_h = CHROME_H + 14;
    L->btn_w = fb_text_width("End task", true) + 36;
    if (L->btn_w < 8 * CHROME_W)
        L->btn_w = 8 * CHROME_W;
    L->btn_x = L->cx + L->cw - L->btn_w;
    L->btn_y = L->cy;

    /* Tick strip sits under the button row so Task Manager itself can
     * show idle samples. Kernel Lab has the same strip; one function
     * draws both so the colours cannot drift. */
    L->trace_h = CHROME_H + 10;
    L->trace_y = L->cy + L->btn_h + 6;
    L->head_y  = L->trace_y + L->trace_h + 4;
    L->row_h   = CHROME_H + 8;
    L->list_x  = L->cx;
    L->list_y  = L->head_y + CHROME_H + 8;
    L->list_w  = L->cw;
    L->status_y = L->cy + L->ch - CHROME_H - 2;

    /* Two summary rows sit between the list and the status line: one for
     * memory, one for the graphics adapter. They are the totals the list
     * cannot show, which is the same split Task Manager makes between its
     * process list and the figures underneath it. */
    L->summary_h = 2 * (CHROME_H + 6);
    L->summary_y = L->status_y - L->summary_h - 6;

    int body = L->summary_y - L->list_y - 8;
    L->rows = body / L->row_h;
    if (L->rows < 1)
        L->rows = 1;

    L->pid_x   = L->list_x + 10;
    L->name_x  = L->pid_x + fb_text_width("PID", true) + 36;
    L->state_x = L->name_x + fb_text_width("worker_cxxxx", true);
    L->cpu_x   = L->state_x + fb_text_width("runningxx", true);
    L->mem_x   = L->cpu_x + fb_text_width("CPU 100%", true) + 16;
    L->ticks_x = L->mem_x + fb_text_width("Memory 9999 KB", true);
}

static task_t *tm_task_at(int row)
{
    int seen = 0;

    if (row < 0)
        return 0;

    for (task_t *t = task_list(); t; t = t->next) {
        if (seen == row)
            return t;
        seen++;
    }
    return 0;
}

static uint32_t tm_recent_pct(int id)
{
    uint32_t ids[SCHED_TRACE_LEN];
    uint32_t n = task_sched_trace(ids, SCHED_TRACE_LEN);
    uint32_t hits = 0;

    if (!n)
        return 0;

    for (uint32_t i = 0; i < n; i++)
        if ((int)ids[i] == id)
            hits++;

    return (hits * 100u) / n;
}

static void draw_tm(const window_t *win, bool active)
{
    tm_layout_t L;
    tm_layout(win, &L);

    int count = task_count();
    if (tm_sel >= count) tm_sel = count - 1;
    if (tm_sel < 0)      tm_sel = 0;
    if (tm_sel < tm_scroll)
        tm_scroll = tm_sel;
    if (tm_sel >= tm_scroll + L.rows)
        tm_scroll = tm_sel - L.rows + 1;
    if (tm_scroll > count - L.rows) tm_scroll = count - L.rows;
    if (tm_scroll < 0)              tm_scroll = 0;

    fb_fill_round_rect(L.btn_x, L.btn_y, L.btn_w, L.btn_h, 4, col_close);
    fb_text_aa(L.btn_x + (L.btn_w - fb_text_width("End task", true)) / 2,
               L.btn_y + (L.btn_h - CHROME_H) / 2, "End task", col_white, true);

    char cap[48];
    int n = 0;
    n += u32_to_str((uint32_t)count, cap + n);
    cap[n++] = ' ';
    cap[n++] = 't'; cap[n++] = 'a'; cap[n++] = 's'; cap[n++] = 'k';
    if (count != 1) cap[n++] = 's';
    cap[n++] = ' '; cap[n++] = ' ';
    n += u32_to_str(task_switch_count(), cap + n);
    cap[n++] = ' ';
    cap[n++] = 's'; cap[n++] = 'w'; cap[n++] = 'i'; cap[n++] = 't';
    cap[n++] = 'c'; cap[n++] = 'h'; cap[n++] = 'e'; cap[n++] = 's';
    cap[n] = '\0';
    fb_text_aa(L.cx, L.btn_y + (L.btn_h - CHROME_H) / 2, cap, col_black, true);

    draw_tick_strip(L.cx, L.trace_y, L.cw, L.trace_h);

    fb_text_aa(L.pid_x, L.head_y, "PID", col_title_off, true);
    fb_text_aa(L.name_x, L.head_y, "Name", col_title_off, true);
    fb_text_aa(L.state_x, L.head_y, "State", col_title_off, true);
    fb_text_aa(L.cpu_x, L.head_y, "CPU", col_title_off, true);
    fb_text_aa(L.mem_x, L.head_y, "Memory", col_title_off, true);
    fb_text_aa(L.ticks_x, L.head_y, "Ticks", col_title_off, true);

    fb_fill_rect(L.list_x, L.list_y, L.list_w, L.rows * L.row_h, col_list_bg);

    for (int r = 0; r < L.rows; r++) {
        task_t *t = tm_task_at(tm_scroll + r);
        if (!t)
            break;

        int y = L.list_y + r * L.row_h;
        bool sel = (tm_scroll + r) == tm_sel;
        uint32_t fg = col_black;

        if (sel) {
            fb_fill_rect(L.list_x, y, L.list_w, L.row_h,
                         active ? col_list_sel : col_list_sel_off);
            fg = active ? col_white : col_black;
        }

        char pid[12], ticks[12], cpu[16], mem[16];
        u32_to_str((uint32_t)t->id, pid);
        u32_to_str(t->ticks, ticks);

        /* A task's memory here is the stack it was given. Nothing tracks
         * which heap blocks belong to which task -- kmalloc has no owner --
         * so reporting a share of the heap would be a number made up to fill
         * a column. The stack is what a task actually holds. */
        int mn = 0;

        if (t->stack_size == 0) {
            /* The kernel task runs on the stack boot.asm set up, which
             * was never allocated and has no recorded size. Printing
             * 0 KB would read as "uses no memory", which is the
             * opposite of true. */
            mem[mn++] = 'b'; mem[mn++] = 'o';
            mem[mn++] = 'o'; mem[mn++] = 't';
        } else {
            mn += u32_to_str(t->stack_size / 1024, mem);
            mem[mn++] = ' '; mem[mn++] = 'K'; mem[mn++] = 'B';
        }

        mem[mn] = '\0';
        int cn = u32_to_str(tm_recent_pct(t->id), cpu);
        cpu[cn++] = '%';
        cpu[cn] = '\0';

        fb_text_aa(L.pid_x, y + (L.row_h - CHROME_H) / 2, pid, fg, true);
        fb_text_aa(L.name_x, y + (L.row_h - CHROME_H) / 2, t->name, fg, true);
        fb_text_aa(L.state_x, y + (L.row_h - CHROME_H) / 2,
                   task_state_name(t->state), fg, true);
        fb_text_aa(L.cpu_x, y + (L.row_h - CHROME_H) / 2, cpu, fg, true);
        fb_text_aa(L.mem_x, y + (L.row_h - CHROME_H) / 2, mem, fg, true);
        fb_text_aa(L.ticks_x, y + (L.row_h - CHROME_H) / 2, ticks, fg, true);
    }

    /* ---- memory ---------------------------------------------------- */
    heap_stats_t heap;
    heap_get_stats(&heap);

    uint32_t used_kb  = heap.used_bytes / 1024;
    uint32_t total_kb = heap.total_bytes / 1024;
    uint32_t pct = total_kb ? (used_kb * 100) / total_kb : 0;

    int bar_x = L.cx + fb_text_width("Memory  ", true);
    int bar_w = L.cw / 3;
    int row_a = L.summary_y;
    int row_b = L.summary_y + CHROME_H + 6;

    fb_text_aa(L.cx, row_a, "Memory", col_title_off, true);

    fb_fill_rect(bar_x, row_a + 3, bar_w, CHROME_H - 4, col_list_sel_off);
    fb_fill_rect(bar_x, row_a + 3, (int)((uint32_t)bar_w * pct / 100),
                 CHROME_H - 4, col_list_sel);

    char mline[64];
    n = 0;
    n += u32_to_str(used_kb, mline + n);
    mline[n++] = ' '; mline[n++] = '/'; mline[n++] = ' ';
    n += u32_to_str(total_kb, mline + n);
    mline[n++] = ' '; mline[n++] = 'K'; mline[n++] = 'B';
    mline[n++] = ' '; mline[n++] = ' '; mline[n++] = '(';
    n += u32_to_str(pct, mline + n);
    mline[n++] = '%'; mline[n++] = ')';
    mline[n] = '\0';
    fb_text_aa(bar_x + bar_w + 12, row_a, mline, col_black, true);

    /* ---- graphics adapter -------------------------------------------- */
    fb_text_aa(L.cx, row_b, "GPU", col_title_off, true);

    char gline[80];
    n = 0;

    if (!svga_available()) {
        const char *none = "no accelerator - software rendering";
        while (*none && n < (int)sizeof(gline) - 1)
            gline[n++] = *none++;
    } else {
        const char *name = "VMware SVGA-II  ";
        while (*name && n < (int)sizeof(gline) - 1)
            gline[n++] = *name++;

        /* What the adapter will actually do for us, rather than what it
         * advertises: 3D is reported but not reachable on this guest. */
        const char *caps = svga_can_copy() ? (svga_can_fill() ? "fill+copy"
                                                              : "copy")
                                           : (svga_can_fill() ? "fill" : "none");
        while (*caps && n < (int)sizeof(gline) - 1)
            gline[n++] = *caps++;

        gline[n++] = ' '; gline[n++] = ' ';
        n += u32_to_str(svga_command_count(), gline + n);

        const char *tail = " commands";
        while (*tail && n < (int)sizeof(gline) - 1)
            gline[n++] = *tail++;
    }

    gline[n] = '\0';
    fb_text_aa(bar_x, row_b, gline, col_black, true);

    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;
    char strip[64];
    n = 0;
    strip[n++] = 'u'; strip[n++] = 'p'; strip[n++] = ' ';
    n += u32_to_str(secs / 3600, strip + n);
    strip[n++] = ':';
    strip[n++] = (char)('0' + (secs / 600) % 6);
    strip[n++] = (char)('0' + (secs / 60) % 10);
    strip[n++] = ':';
    strip[n++] = (char)('0' + (secs / 10) % 6);
    strip[n++] = (char)('0' + secs % 10);
    {
        const char *hint = "  grey = idle";
        int i = 0;
        while (hint[i] && n < (int)sizeof(strip) - 1)
            strip[n++] = hint[i++];
    }
    if (tm_msg[0]) {
        strip[n++] = ' ';
        strip[n++] = '-';
        strip[n++] = ' ';
        int i = 0;
        while (tm_msg[i] && n < (int)sizeof(strip) - 1)
            strip[n++] = tm_msg[i++];
    }
    strip[n] = '\0';
    fb_text_aa(L.cx, L.status_y, strip, col_title_off, true);
}

static void tm_end_selected(void)
{
    task_t *t = tm_task_at(tm_sel);

    if (!t) {
        strcpy(tm_msg, "no task selected");
        return;
    }
    if (t->id == KERNEL_TASK_ID || t == task_current() || task_is_idle(t)) {
        strcpy(tm_msg, task_is_idle(t) ? "the idle task cannot be ended"
                                       : "the kernel task cannot be ended");
        return;
    }
    if (t->state == TASK_DEAD) {
        strcpy(tm_msg, "already ended");
        return;
    }

    char name[TASK_NAME_LEN];
    strncpy(name, t->name, TASK_NAME_LEN);
    name[TASK_NAME_LEN - 1] = '\0';
    task_kill(t);
    task_reap();
    strcpy(tm_msg, "ended ");
    int i = 0;
    int n = (int)strlen(tm_msg);
    while (name[i] && n < (int)sizeof(tm_msg) - 1)
        tm_msg[n++] = name[i++];
    tm_msg[n] = '\0';
}

static bool tm_click(const window_t *win, int mx, int my)
{
    tm_layout_t L;
    tm_layout(win, &L);

    if (in_rect(mx, my, L.btn_x, L.btn_y, L.btn_w, L.btn_h)) {
        tm_end_selected();
        return true;
    }

    if (!in_rect(mx, my, L.list_x, L.list_y, L.list_w, L.rows * L.row_h))
        return false;

    int row = tm_scroll + (my - L.list_y) / L.row_h;
    if (!tm_task_at(row))
        return false;

    tm_sel = row;
    tm_msg[0] = '\0';
    return true;
}

/* ---- notepad -------------------------------------------------------------
 *
 * A plain-text buffer over the filesystem. Keys reach here only while
 * this window is focused, which is what stops them falling into the
 * graphical terminal. The buffer is FS_MAX_SIZE so a save cannot promise
 * more than the filesystem will accept. Save is write-through: if a disk
 * is present, fs_write has already flushed before we report success. */

static char     np_text[FS_MAX_SIZE + 1];
static int      np_len;
static int      np_cursor;
static int      np_scroll;              /* first visible line */
static char     np_name[FS_NAME_MAX];
static char     np_saved_name[FS_NAME_MAX]; /* last name on disk; empty if never saved */
static char     np_edit_name[FS_NAME_MAX];
static int      np_edit_cursor;
static bool     np_naming;
static bool     np_dirty;
static bool     np_picker;
static int      np_pick_sel;
static int      np_pick_scroll;
static char     np_msg[64];

typedef struct {
    int cx, cy, cw, ch;
    int bar_y, bar_h;
    int n_btns;
    int btns_x[3], btns_w[3], btn_h;
    int name_x, name_w;
    int text_x, text_y, text_w, text_h;
    int cols, rows;
    int status_y;
} np_layout_t;

static void np_layout(const window_t *win, np_layout_t *L)
{
    static const char *labels[3] = { "New", "Open", "Save" };

    L->cx = win->x + BORDER + 8;
    L->cy = win->y + TITLE_H + BORDER + 8;
    L->cw = win->w - 2 * (BORDER + 8);
    L->ch = win->h - TITLE_H - 2 * (BORDER + 8);

    L->bar_y = L->cy;
    L->btn_h = CHROME_H + 12;
    L->bar_h = L->btn_h;
    L->n_btns = 3;

    int x = L->cx;
    for (int i = 0; i < 3; i++) {
        int w = fb_text_width(labels[i], true) + 28;
        L->btns_x[i] = x;
        L->btns_w[i] = w;
        x += w + 8;
    }

    L->name_x = x + 8;
    L->name_w = L->cx + L->cw - L->name_x;
    if (L->name_w < 8 * CHROME_W)
        L->name_w = 8 * CHROME_W;

    L->status_y = L->cy + L->ch - CHROME_H - 2;
    L->text_x = L->cx;
    L->text_y = L->cy + L->bar_h + 8;
    L->text_w = L->cw;
    L->text_h = L->status_y - L->text_y - 8;
    if (L->text_h < CELL_H)
        L->text_h = CELL_H;

    L->cols = L->text_w / CELL_W;
    L->rows = L->text_h / CELL_H;
    if (L->cols < 8) L->cols = 8;
    if (L->rows < 3) L->rows = 3;
}

static int np_line_count(void)
{
    int lines = 1;
    for (int i = 0; i < np_len; i++)
        if (np_text[i] == '\n')
            lines++;
    return lines;
}

static int np_line_start(int line)
{
    int i = 0, l = 0;
    while (i < np_len && l < line) {
        if (np_text[i] == '\n')
            l++;
        i++;
    }
    return i;
}

static int np_cursor_line(void)
{
    int line = 0;
    for (int i = 0; i < np_cursor && i < np_len; i++)
        if (np_text[i] == '\n')
            line++;
    return line;
}

static int np_cursor_col(void)
{
    int start = np_line_start(np_cursor_line());
    return np_cursor - start;
}

static void np_move_vert(int delta)
{
    int line = np_cursor_line() + delta;
    int lines = np_line_count();
    int col = np_cursor_col();

    if (line < 0) line = 0;
    if (line >= lines) line = lines - 1;

    int start = np_line_start(line);
    int end = start;
    while (end < np_len && np_text[end] != '\n')
        end++;

    int width = end - start;
    if (col > width)
        col = width;
    np_cursor = start + col;
}

static void np_insert(char c)
{
    if (np_len >= FS_MAX_SIZE) {
        strcpy(np_msg, "file is at the 64 KB limit");
        return;
    }
    memmove(np_text + np_cursor + 1, np_text + np_cursor,
            (size_t)(np_len - np_cursor));
    np_text[np_cursor] = c;
    np_len++;
    np_cursor++;
    np_text[np_len] = '\0';
    np_dirty = true;
    np_msg[0] = '\0';
}

static void np_backspace(void)
{
    if (np_cursor <= 0)
        return;
    memmove(np_text + np_cursor - 1, np_text + np_cursor,
            (size_t)(np_len - np_cursor));
    np_cursor--;
    np_len--;
    np_text[np_len] = '\0';
    np_dirty = true;
    np_msg[0] = '\0';
}

static void notepad_open_named(const char *name)
{
    const fs_file_t *f = fs_find(name);

    np_picker = false;
    np_naming = false;
    np_msg[0] = '\0';
    strncpy(np_name, name, FS_NAME_MAX);
    np_name[FS_NAME_MAX - 1] = '\0';
    strncpy(np_saved_name, np_name, FS_NAME_MAX);
    np_saved_name[FS_NAME_MAX - 1] = '\0';

    if (!f || !f->data) {
        np_len = 0;
        np_text[0] = '\0';
        np_cursor = 0;
        np_scroll = 0;
        np_dirty = false;
        return;
    }

    uint32_t n = f->size;
    if (n > FS_MAX_SIZE)
        n = FS_MAX_SIZE;
    memcpy(np_text, f->data, n);
    np_len = (int)n;
    np_text[np_len] = '\0';
    np_cursor = 0;
    np_scroll = 0;
    np_dirty = false;
}

static void notepad_new(void)
{
    char name[FS_NAME_MAX];
    strcpy(name, "untitled.txt");

    if (fs_find(name)) {
        int n;
        for (n = 2; n < 99; n++) {
            int p = 0;
            const char *pre = "untitled";
            while (*pre)
                name[p++] = *pre++;
            if (n >= 10)
                name[p++] = (char)('0' + n / 10);
            name[p++] = (char)('0' + n % 10);
            name[p++] = '.';
            name[p++] = 't';
            name[p++] = 'x';
            name[p++] = 't';
            name[p] = '\0';
            if (!fs_find(name))
                break;
        }
    }

    if (!fs_find(name) && fs_file_count() >= FS_MAX_FILES) {
        strcpy(np_msg, "filesystem is full");
        return;
    }

    strncpy(np_name, name, FS_NAME_MAX);
    np_name[FS_NAME_MAX - 1] = '\0';
    np_saved_name[0] = '\0';    /* not on disk until Save */
    np_naming = false;
    np_len = 0;
    np_text[0] = '\0';
    np_cursor = 0;
    np_scroll = 0;
    np_dirty = true;
    np_picker = false;
    np_msg[0] = '\0';
}

static bool np_name_char_ok(char c)
{
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '.' || c == '-' || c == '_';
}

static void np_begin_rename(void)
{
    const char *s = np_name[0] ? np_name : "untitled.txt";
    int n = 0;

    while (s[n] && n < FS_NAME_MAX - 1) {
        np_edit_name[n] = s[n];
        n++;
    }
    np_edit_name[n] = '\0';
    np_edit_cursor = n;
    np_naming = true;
    np_picker = false;
    np_msg[0] = '\0';
}

static void np_cancel_rename(void)
{
    np_naming = false;
    np_edit_name[0] = '\0';
}

static bool np_commit_rename(void)
{
    int n = 0;

    while (np_edit_name[n])
        n++;
    if (n == 0) {
        strcpy(np_msg, "name cannot be empty");
        return false;
    }

    if (strcmp(np_name, np_edit_name) != 0)
        np_dirty = true;

    strncpy(np_name, np_edit_name, FS_NAME_MAX);
    np_name[FS_NAME_MAX - 1] = '\0';
    np_naming = false;
    return true;
}

static void notepad_save(void)
{
    if (np_naming && !np_commit_rename())
        return;

    if (!np_name[0])
        strcpy(np_name, "untitled.txt");

    if (!fs_write(np_name, np_text, (uint32_t)np_len)) {
        const char *e = fs_error();
        if (e && e[0]) {
            const char *pre = "save failed: ";
            int i = 0, j = 0;
            while (pre[i] && i < (int)sizeof(np_msg) - 1)
                np_msg[i] = pre[i], i++;
            while (e[j] && i < (int)sizeof(np_msg) - 1)
                np_msg[i++] = e[j++];
            np_msg[i] = '\0';
        } else {
            strcpy(np_msg, "save failed (full, or over 64 KB)");
        }
        return;
    }

    /* True rename: the user changed the name and saved. The old file is
     * removed only after the new one is on disk, and only if it is a
     * different name that this buffer previously owned. */
    if (np_saved_name[0] && strcmp(np_saved_name, np_name) != 0)
        fs_delete(np_saved_name);

    strncpy(np_saved_name, np_name, FS_NAME_MAX);
    np_saved_name[FS_NAME_MAX - 1] = '\0';
    np_dirty = false;
    strcpy(np_msg, fs_on_disk() ? "saved to disk" : "saved (RAM only)");
}

static void draw_np(const window_t *win, bool active)
{
    np_layout_t L;
    np_layout(win, &L);

    static const char *labels[3] = { "New", "Open", "Save" };

    for (int i = 0; i < 3; i++) {
        fb_fill_round_rect(L.btns_x[i], L.bar_y, L.btns_w[i], L.btn_h, 4,
                           col_btn_on);
        fb_text_aa(L.btns_x[i] + 10,
                   L.bar_y + (L.btn_h - CHROME_H) / 2,
                   labels[i], col_white, true);
    }

    char title[FS_NAME_MAX + 8];
    int n = 0;
    const char *shown = np_naming ? np_edit_name
                                  : (np_name[0] ? np_name : "untitled.txt");

    fb_fill_round_rect(L.name_x, L.bar_y, L.name_w, L.btn_h, 4,
                       np_naming ? col_paper : col_head);
    while (shown[n] && n < FS_NAME_MAX - 1) {
        title[n] = shown[n];
        n++;
    }
    if (!np_naming && np_dirty) {
        title[n++] = ' ';
        title[n++] = '*';
    }
    title[n] = '\0';
    fb_text_aa(L.name_x + 10,
               L.bar_y + (L.btn_h - CHROME_H) / 2, title, col_black, true);

    if (np_naming && active) {
        int cw = fb_text_width(np_edit_name, true);
        int prefix = 0;
        char tmp[FS_NAME_MAX];
        int k = 0;
        while (k < np_edit_cursor && np_edit_name[k]) {
            tmp[k] = np_edit_name[k];
            k++;
        }
        tmp[k] = '\0';
        prefix = fb_text_width(tmp, true);
        (void)cw;
        fb_fill_rect(L.name_x + 10 + prefix,
                     L.bar_y + (L.btn_h - CHROME_H) / 2 + CHROME_H - 2,
                     2, 2, col_black);
    }

    fb_fill_rect(L.text_x, L.text_y, L.text_w, L.text_h, col_paper);

    if (np_picker) {
        int count = fs_file_count();
        int row_h = CHROME_H + 8;
        int rows = L.text_h / row_h;
        if (rows < 1) rows = 1;
        if (np_pick_sel >= count) np_pick_sel = count - 1;
        if (np_pick_sel < 0)      np_pick_sel = 0;
        if (np_pick_sel < np_pick_scroll)
            np_pick_scroll = np_pick_sel;
        if (np_pick_sel >= np_pick_scroll + rows)
            np_pick_scroll = np_pick_sel - rows + 1;
        if (np_pick_scroll < 0) np_pick_scroll = 0;

        fb_text_aa(L.text_x + 8, L.text_y + 4, "Open a file", col_title_off, true);

        for (int r = 0; r < rows; r++) {
            const fs_file_t *f = file_by_row(np_pick_scroll + r);
            if (!f)
                break;
            int y = L.text_y + 4 + CHROME_H + 8 + r * row_h;
            bool sel = (np_pick_scroll + r) == np_pick_sel;
            if (sel)
                fb_fill_rect(L.text_x, y, L.text_w, row_h, col_list_sel);
            fb_text_aa(L.text_x + 12, y + (row_h - CHROME_H) / 2, f->name,
                       sel ? col_white : col_black, true);
        }
    } else {
        int lines = np_line_count();
        int cur_line = np_cursor_line();
        if (cur_line < np_scroll)
            np_scroll = cur_line;
        if (cur_line >= np_scroll + L.rows)
            np_scroll = cur_line - L.rows + 1;
        if (np_scroll > lines - L.rows) np_scroll = lines - L.rows;
        if (np_scroll < 0)              np_scroll = 0;

        for (int r = 0; r < L.rows; r++) {
            int line = np_scroll + r;
            if (line >= lines)
                break;
            int start = np_line_start(line);
            int x = L.text_x + 4;
            int y = L.text_y + 4 + r * CELL_H;
            int col = 0;
            for (int i = start; i < np_len && np_text[i] != '\n'; i++) {
                if (col >= L.cols)
                    break;
                char c = np_text[i];
                if (c >= 32 && c < 127)
                    fb_char_aa(x + col * CELL_W, y, c, col_black, false);
                col++;
            }
        }

        if (active) {
            int cl = np_cursor_line() - np_scroll;
            int cc = np_cursor_col();
            if (cl >= 0 && cl < L.rows && cc < L.cols) {
                fb_fill_rect(L.text_x + 4 + cc * CELL_W,
                             L.text_y + 4 + cl * CELL_H + CELL_H - 2,
                             CELL_W, 2, col_black);
            }
        }
    }

    char st[80];
    n = 0;
    if (np_msg[0]) {
        while (np_msg[n] && n < 60) {
            st[n] = np_msg[n];
            n++;
        }
    } else {
        n += u32_to_str((uint32_t)np_len, st + n);
        st[n++] = ' ';
        st[n++] = 'b'; st[n++] = 'y'; st[n++] = 't'; st[n++] = 'e';
        st[n++] = 's';
    }
    st[n] = '\0';
    fb_text_aa(L.cx, L.status_y, st, col_title_off, true);
}

static bool np_click(const window_t *win, int mx, int my)
{
    np_layout_t L;
    np_layout(win, &L);

    for (int i = 0; i < 3; i++) {
        if (!in_rect(mx, my, L.btns_x[i], L.bar_y, L.btns_w[i], L.btn_h))
            continue;
        if (i == 0)
            notepad_new();
        else if (i == 1) {
            np_picker = true;
            np_naming = false;
            np_pick_sel = 0;
            np_pick_scroll = 0;
            np_msg[0] = '\0';
        } else
            notepad_save();
        return true;
    }

    if (in_rect(mx, my, L.name_x, L.bar_y, L.name_w, L.btn_h)) {
        np_begin_rename();
        return true;
    }

    if (np_picker && in_rect(mx, my, L.text_x, L.text_y, L.text_w, L.text_h)) {
        int row_h = CHROME_H + 8;
        int y0 = L.text_y + 4 + CHROME_H + 8;
        if (my >= y0) {
            int row = np_pick_scroll + (my - y0) / row_h;
            const fs_file_t *f = file_by_row(row);
            if (f) {
                np_pick_sel = row;
                notepad_open_named(f->name);
            }
        }
        return true;
    }

    if (!np_picker && in_rect(mx, my, L.text_x, L.text_y, L.text_w, L.text_h)) {
        int line = np_scroll + (my - (L.text_y + 4)) / CELL_H;
        int col  = (mx - (L.text_x + 4)) / CELL_W;
        int lines = np_line_count();
        if (line < 0) line = 0;
        if (line >= lines) line = lines - 1;
        int start = np_line_start(line);
        int end = start;
        while (end < np_len && np_text[end] != '\n')
            end++;
        if (col < 0) col = 0;
        if (col > end - start) col = end - start;
        np_cursor = start + col;
        return true;
    }

    return false;
}

static bool np_key(char c)
{
    if (np_naming) {
        if (c == 27) {
            np_cancel_rename();
            return true;
        }
        if (c == '\n') {
            np_commit_rename();
            return true;
        }
        if (c == KEY_LEFT) {
            if (np_edit_cursor > 0)
                np_edit_cursor--;
            return true;
        }
        if (c == KEY_RIGHT) {
            if (np_edit_name[np_edit_cursor])
                np_edit_cursor++;
            return true;
        }
        if (c == '\b') {
            if (np_edit_cursor > 0) {
                int i = np_edit_cursor - 1;
                while (np_edit_name[i]) {
                    np_edit_name[i] = np_edit_name[i + 1];
                    i++;
                }
                np_edit_cursor--;
            }
            return true;
        }
        if (c >= 32 && c < 127) {
            int len = 0;
            while (np_edit_name[len])
                len++;
            if (len >= FS_NAME_MAX - 1) {
                strcpy(np_msg, "name is at the 31-character limit");
                return true;
            }
            if (!np_name_char_ok(c)) {
                strcpy(np_msg, "letters, digits, . - _ only");
                return true;
            }
            for (int i = len; i >= np_edit_cursor; i--)
                np_edit_name[i + 1] = np_edit_name[i];
            np_edit_name[np_edit_cursor] = c;
            np_edit_cursor++;
            np_msg[0] = '\0';
            return true;
        }
        return true;
    }

    if (np_picker) {
        if (c == 27) {
            np_picker = false;
            return true;
        }
        if (c == KEY_UP && np_pick_sel > 0)
            np_pick_sel--;
        else if (c == KEY_DOWN)
            np_pick_sel++;
        else if (c == '\n') {
            const fs_file_t *f = file_by_row(np_pick_sel);
            if (f)
                notepad_open_named(f->name);
        }
        return true;
    }

    if (c == KEY_LEFT) {
        if (np_cursor > 0)
            np_cursor--;
    } else if (c == KEY_RIGHT) {
        if (np_cursor < np_len)
            np_cursor++;
    } else if (c == KEY_UP) {
        np_move_vert(-1);
    } else if (c == KEY_DOWN) {
        np_move_vert(1);
    } else if (c == KEY_PGUP) {
        np_move_vert(-8);
    } else if (c == KEY_PGDN) {
        np_move_vert(8);
    } else if (c == '\b') {
        np_backspace();
    } else if (c == '\n') {
        np_insert('\n');
    } else if (c >= 32 && c < 127) {
        np_insert(c);
    } else {
        return false;
    }
    return true;
}

/* ---- kernel lab ----------------------------------------------------------
 *
 * Every experiment in here already existed as a shell command, and that
 * was the problem: showing this system to somebody meant typing at them.
 * This window puts the same primitives behind buttons — the sidebar lists
 * the experiments, the pane explains the selected one, Run runs it, and
 * the verdict is drawn rather than read out of a scrollback.
 *
 * Nothing is reimplemented. Each entry calls the same demos.h primitive the
 * shell command and the self-test call, so what this window runs and what
 * the machine verifies cannot drift apart.
 */

typedef enum {
    ENTRY_HEADER,           /* a section label, not clickable */
    ENTRY_SPAWN,
    ENTRY_PREEMPT,
    ENTRY_PREEMPTJOB,       /* slot 0: hogged, slot 1: sharing */
    ENTRY_BG,
    ENTRY_SCHED,            /* live timeline of the real scheduler */
    ENTRY_RACE,             /* slot 0: no lock, slot 1: mutex */
    ENTRY_FILERACE,         /* slot 0: no lock, slot 1: mutex, real file */
    ENTRY_THREADS,          /* slot 0: sequential, slot 1: overlapping */
    ENTRY_PRODCONS,
    ENTRY_DEADLOCK,
    ENTRY_MMU,
    ENTRY_FAULT,            /* arg names the exception to raise */
    ENTRY_USER,             /* slot 0: direct hardware, slot 1: syscall */
    ENTRY_GPUTEST,
    ENTRY_LIVE_TASKS,       /* the LIVE entries have no Run button; their */
    ENTRY_LIVE_MEM,         /* pane is redrawn from kernel state on every */
    ENTRY_LIVE_PCI,         /* periodic frame, so it is always current    */
    ENTRY_LIVE_GPU,
} demo_kind_t;

typedef struct {
    demo_kind_t kind;
    const char *label;      /* sidebar text */
    const char *arg;        /* fault kind for ENTRY_FAULT, else unused */
    int         slot;       /* which result slot this entry reports into */
    const char *desc[4];    /* pane description, unused lines are null */
} demo_entry_t;

static const demo_entry_t demo_entries[] = {
    { ENTRY_HEADER, "Scheduler", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_PREEMPTJOB, "Hog vs Notepad", 0, 0,
      { "A hog burns the CPU and never waits. Sharing is the timer.",
        "Off: Notepad freezes ~3s. On: type while hog Ticks climb.", 0, 0 } },
    { ENTRY_BG, "Background workers", 0, 0,
      { "Start workers, open Task Manager, watch ticks rise.",
        "They wait their turn, so they will not freeze Notepad.", 0, 0 } },
    { ENTRY_SPAWN, "Preemption proof", 0, 0,
      { "Three tasks print their letter in tight loops",
        "and never yield. Interleaving is the timer switching.", 0, 0 } },
    { ENTRY_PREEMPT, "Preemption switch", 0, 0,
      { "Turn the timer off and a running task keeps the CPU",
        "until it finishes. Turn it back on afterwards.", 0, 0 } },
    { ENTRY_SCHED, "Scheduler timeline", 0, 0,
      { "Who had the CPU on each of the last timer ticks.",
        "This strip is recorded from the tick path itself.", 0, 0 } },
    { ENTRY_THREADS, "Threads, sequential", 0, 0,
      { "Eight jobs, each a wait then a spin, one after another.",
        "Then run overlapping: waits can share the CPU.", 0, 0 } },
    { ENTRY_THREADS, "Threads, overlapping", 0, 1,
      { "The same eight jobs on two tasks. One CPU: the win is",
        "overlapping a wait, not two computes at once.", 0, 0 } },

    { ENTRY_HEADER, "Files", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_FILERACE, "Two programs, one file (unlocked)", 0, 0,
      { "Two programs write till.log at once. No lock: letters tear.",
        "Open it in Notepad. The strip below is the same bytes.", 0, 0 } },
    { ENTRY_FILERACE, "Two programs, one file (locked)", 0, 1,
      { "Same two programs, a lock around each full line.",
        "Every line in till.log is whole. Open it.", 0, 0 } },

    { ENTRY_HEADER, "Sync", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_RACE, "Till, no lock", 0, 0,
      { "Two cashiers, one till. They both read the same old",
        "total, then both write. The till comes up short.", 0, 0 } },
    { ENTRY_RACE, "Till, with lock", 0, 1,
      { "Same two cashiers, a lock so only one is at the till.",
        "Every run the total is exactly 100.", 0, 0 } },
    { ENTRY_PRODCONS, "Producer / consumer", 0, 0,
      { "A 4-slot ring. The writer stops when it is full.",
        "The boxes and FULL banner are the real buffer.", 0, 0 } },
    { ENTRY_DEADLOCK, "Deadlock", 0, 0,
      { "Two tasks, two locks, opposite order. Drag this window:",
        "they are stuck; the rest of the machine is not.", 0, 0 } },

    { ENTRY_HEADER, "Memory", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_MMU, "MMU / paging", 0, 0,
      { "The real page directory. The bar is RAM and",
        "device memory coloured by region. Walk walks",
        "one virtual address through directory, table",
        "and frame - including the unmapped ones." } },

    { ENTRY_HEADER, "Robustness", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_FAULT, "Fault: divide by zero", "div0", 0,
      { "A spawned task divides by zero - a real CPU",
        "exception, vector 0. The handler kills that",
        "task; the kernel and this desktop survive.", 0 } },
    { ENTRY_FAULT, "Fault: bad opcode", "opcode", 1,
      { "The task executes ud2, an instruction that",
        "does not exist. Exception 6, task killed,",
        "everything else keeps running.", 0 } },
    { ENTRY_FAULT, "Fault: protection", "gpf", 2,
      { "The task loads a segment selector past the",
        "end of the GDT. General protection fault,",
        "error code included in the dump below.", 0 } },
    { ENTRY_FAULT, "Fault: null pointer", "null", 3,
      { "The task dereferences address zero. The CPU",
        "reports the faulting address in CR2 - a value",
        "the kernel never assigned.", 0 } },
    { ENTRY_FAULT, "Fault: unmapped page", "page", 4,
      { "The task reads an address no page table maps.",
        "Page fault, CR2 names the address, and only",
        "the offending task dies.", 0 } },
    { ENTRY_USER, "Ring 3: touch hardware", 0, 0,
      { "A task drops to ring 3 and writes to a VGA",
        "port directly. IOPL is 0, so the CPU refuses",
        "with a protection fault - unprivileged code",
        "cannot reach hardware." } },
    { ENTRY_USER, "Ring 3: via syscall", 0, 1,
      { "The same unprivileged task asks the kernel",
        "through int 0x80 instead. Same privilege",
        "level, legal route, clean exit.", 0 } },

    { ENTRY_HEADER, "Graphics", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_GPUTEST, "GPU vs CPU fill", 0, 0,
      { "Fill the whole screen 60 times with the CPU,",
        "then 60 times as six-word commands in the",
        "adapter's FIFO. The screen flashes while it",
        "runs; the bars below compare the cost." } },

    { ENTRY_HEADER, "Live view", 0, 0, { 0, 0, 0, 0 } },
    { ENTRY_LIVE_TASKS, "Task list", 0, 0,
      { "Every task the scheduler knows, refreshed",
        "live. Ticks is CPU time received.", 0, 0 } },
    { ENTRY_LIVE_MEM, "Memory", 0, 0,
      { "The kernel heap: a first-fit free list with",
        "splitting and coalescing, checked live.", 0, 0 } },
    { ENTRY_LIVE_PCI, "PCI bus", 0, 0,
      { "Devices discovered on the PCI bus at boot.",
        "Their addresses are assigned by the firmware,",
        "not assumed by the kernel.", 0 } },
    { ENTRY_LIVE_GPU, "Display adapter", 0, 0,
      { "The graphics adapter, if one was found, and",
        "which accelerated operations it offers.", 0, 0 } },
};

#define DEMO_ENTRY_COUNT ((int)(sizeof(demo_entries) / sizeof(demo_entries[0])))

/* Results survive deselection so a viewer can flip between demonstrations and
 * still see what each one produced. Only the most recent run of each is kept:
 * a history would need scrolling, and the terminal already provides that. */
#define DEMO_RACE_RUNS 3

static struct { bool ran; bool preempt_on; uint32_t switches; } spawn_result;
static struct { bool ran; int runs; uint32_t total[DEMO_RACE_RUNS]; } race_result[2];
static struct { bool ran; uint32_t ticks; } threads_result[2];
static struct { bool ran; bool ok; } pc_result;
static struct { bool ran; int before, after; } fault_result[5];
static struct { bool ran; bool completed; } user_result[2];
static struct { bool ran; uint32_t cpu_ticks, gpu_ticks; } gpu_result;

static int mmu_walk_sel;
static bool mmu_walked;
static page_walk_t mmu_last;

static int demo_sel = 1;    /* the first entry is a header */
static int demo_scroll;

/* Output log. Demonstrations print through kprintf, and while one runs the
 * console sink is pointed here instead of at the terminal window, so the
 * fault dumps and interleaved letters land next to the button that caused
 * them. The grid works like the terminal's, minus scrollback: each run
 * starts clean, and the verdict banner carries the conclusion. */
#define DLOG_MAX_ROWS 18
#define DLOG_MAX_COLS 100

static char     dlog[DLOG_MAX_ROWS][DLOG_MAX_COLS];
static int      dlog_rows, dlog_cols;   /* used portion, sized in gui_run */
static int      dlog_cx, dlog_cy;
static uint32_t dlog_last_flush;

static void dlog_clear(void)
{
    memset(dlog, ' ', sizeof(dlog));
    dlog_cx = dlog_cy = 0;
}

static void dlog_scroll(void)
{
    for (int y = 1; y < dlog_rows; y++)
        memcpy(dlog[y - 1], dlog[y], (uint32_t)dlog_cols);

    memset(dlog[dlog_rows - 1], ' ', (uint32_t)dlog_cols);
    dlog_cy = dlog_rows - 1;
}

static void dlog_putc(char c)
{
    if (dlog_rows <= 0)
        return;

    if (c == '\n')      { dlog_cx = 0; dlog_cy++; }
    else if (c == '\r') { dlog_cx = 0; }
    else if (c == '\t') { dlog_cx = (dlog_cx + 4) & ~3; }
    else if (c == '\b') {
        if (dlog_cx > 0) dlog[dlog_cy][--dlog_cx] = ' ';
    } else if (c >= 32) {
        dlog[dlog_cy][dlog_cx++] = c;
    }

    if (dlog_cx >= dlog_cols) { dlog_cx = 0; dlog_cy++; }
    if (dlog_cy >= dlog_rows) dlog_scroll();

    /* Same reasoning as term_putc: the demonstrations run synchronously
     * inside the render loop, and several print no newline for seconds at a
     * time, so the flush has to happen on elapsed time as well as on line
     * boundaries or the output arrives all at once at the end. */
    if (gui_active && (c == '\n' || pit_ticks() - dlog_last_flush >= 10))
        gui_flush_demos();
}

/* Assemble strings by hand, as the file manager does: there is no snprintf
 * here and kprintf writes to the console, not to a buffer. */
static int str_append(char *out, int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    out[n] = '\0';

    return n;
}

static int hex_append(char *out, int n, uint32_t v, int digits)
{
    static const char hexdig[] = "0123456789abcdef";

    for (int i = digits - 1; i >= 0; i--)
        out[n++] = hexdig[(v >> (i * 4)) & 0xF];
    out[n] = '\0';

    return n;
}

/* The workers announce themselves only by existing, so the button's label and
 * the status lamp both come from the task list rather than from a flag that
 * could disagree with it — the terminal can start and stop workers too. */
static int demo_worker_count(void)
{
    int n = 0;

    for (task_t *t = task_list(); t; t = t->next)
        if (t->state != TASK_DEAD && strncmp(t->name, "worker_", 7) == 0)
            n++;

    return n;
}

static int demo_desc_lines(const demo_entry_t *e)
{
    int n = 0;

    while (n < 4 && e->desc[n])
        n++;

    return n;
}

static bool demo_entry_runnable(const demo_entry_t *e)
{
    return e->kind != ENTRY_HEADER
        && e->kind != ENTRY_LIVE_TASKS
        && e->kind != ENTRY_LIVE_MEM
        && e->kind != ENTRY_LIVE_PCI
        && e->kind != ENTRY_LIVE_GPU;
}

static bool demo_entry_has_log(const demo_entry_t *e)
{
    switch (e->kind) {
    case ENTRY_MMU:
    case ENTRY_SCHED:
    case ENTRY_PRODCONS:
    case ENTRY_DEADLOCK:
        return false;
    default:
        return demo_entry_runnable(e);
    }
}

static bool demo_entry_enabled(const demo_entry_t *e)
{
    /* The benchmark needs something to benchmark. Without the accelerated
     * adapter the button is drawn disabled and says why, rather than being
     * hidden and leaving the viewer wondering what would have been here. */
    if (e->kind == ENTRY_GPUTEST)
        return svga_can_fill();

    return true;
}

#define DEMO_MAX_BTNS 3

static int demo_fill_buttons(const demo_entry_t *e, const char **labels)
{
    switch (e->kind) {
    case ENTRY_PREEMPT:
        labels[0] = task_preempt_enabled() ? "Turn preemption off"
                                           : "Turn preemption on";
        return 1;
    case ENTRY_BG:
        labels[0] = demo_worker_count() > 0 ? "Stop the workers"
                                            : "Start 3 workers";
        return 1;
    case ENTRY_SCHED:
        labels[0] = demo_worker_count() > 0 ? "Stop workers" : "Start workers";
        labels[1] = task_preempt_enabled() ? "Preempt off" : "Preempt on";
        return 2;
    case ENTRY_RACE:
        labels[0] = e->slot == 1 ? "Run with lock" : "Run unlocked";
        return 1;
    case ENTRY_PREEMPTJOB: {
        desktop_hog_info_t inf;
        desktop_hog_snapshot(&inf);
        if (inf.running) {
            labels[0] = "Hog running...";
            return 1;
        }
        labels[0] = "Run with sharing OFF";
        labels[1] = "Run with sharing ON";
        return 2;
    }
    case ENTRY_FILERACE:
        labels[0] = e->slot == 1 ? "Write with lock" : "Write unlocked";
        return 1;
    case ENTRY_PRODCONS:
        labels[0] = pc_live_running() ? "Stop" : "Start live run";
        return 1;
    case ENTRY_DEADLOCK: {
        deadlock_info_t d;
        deadlock_snapshot(&d);
        labels[0] = "Trigger deadlock";
        labels[1] = "Ordered locks";
        labels[2] = d.deadlocked ? "Kill victim" : "Reset";
        return 3;
    }
    case ENTRY_MMU:
        labels[0] = "Walk next address";
        labels[1] = "Fault this address";
        return 2;
    case ENTRY_GPUTEST:
        labels[0] = "Run benchmark";
        return 1;
    default:
        labels[0] = "Run demo";
        return demo_entry_runnable(e) ? 1 : 0;
    }
}

typedef struct {
    int cx, cy, cw, ch;             /* content box inside the window frame */
    int list_x, list_y, list_w;     /* sidebar */
    int row_h, rows;
    int pane_x, pane_y, pane_w;     /* everything right of the sidebar */
    int desc_y;
    int n_btns;
    int btns_x[DEMO_MAX_BTNS];
    int btns_w[DEMO_MAX_BTNS];
    int btn_x, btn_y, btn_w, btn_h; /* first button; also top of a live panel */
    int result_y;
    int log_x, log_y, log_w, log_h; /* captured-output box */
    int strip_y;                    /* live status line along the bottom */
    int pane_bottom;                /* pane content must stop above this */
} demos_layout_t;

/* Drawing and hit-testing share this, for the same reason the file manager's
 * layout is shared: a Run button drawn from one set of expressions and
 * clicked with another eventually runs the demonstration above the one the
 * pointer was on. */
static void demos_layout(const window_t *win, demos_layout_t *L)
{
    L->cx = win->x + BORDER + 8;
    L->cy = win->y + TITLE_H + BORDER + 8;
    L->cw = win->w - 2 * (BORDER + 8);
    L->ch = win->h - TITLE_H - 2 * BORDER - 16;

    L->strip_y = L->cy + L->ch - CHROME_H - 2;

    /* The sidebar is sized to its longest label, measured once — the labels
     * are compile-time constants and so is the font. */
    static int widest;

    if (!widest)
        for (int i = 0; i < DEMO_ENTRY_COUNT; i++) {
            int w = fb_text_width(demo_entries[i].label, true);
            if (w > widest)
                widest = w;
        }

    L->list_w = (L->cw * 32) / 100;
    if (L->list_w < widest + 32)
        L->list_w = widest + 32;
    if (L->list_w > (L->cw * 45) / 100)
        L->list_w = (L->cw * 45) / 100;

    L->list_x = L->cx;
    L->list_y = L->cy;
    L->row_h  = CHROME_H + 8;

    L->rows = (L->strip_y - 10 - L->list_y) / L->row_h;
    if (L->rows < 3)
        L->rows = 3;
    if (L->rows > DEMO_ENTRY_COUNT)
        L->rows = DEMO_ENTRY_COUNT;

    L->pane_x = L->cx + L->list_w + 14;
    L->pane_y = L->cy;
    L->pane_w = L->cx + L->cw - L->pane_x;
    L->pane_bottom = L->strip_y - 10;

    const demo_entry_t *e = &demo_entries[demo_sel];

    L->desc_y = L->pane_y + CHROME_H + 14;

    const char *labels[DEMO_MAX_BTNS];
    int n = demo_fill_buttons(e, labels);
    L->n_btns = n;
    L->btn_h = CHROME_H + 14;
    L->btn_y = L->desc_y + demo_desc_lines(e) * (CHROME_H + 4) + 8;

    int x = L->pane_x;
    for (int i = 0; i < n; i++) {
        int w = fb_text_width(labels[i], true) + 36;
        if (w < 8 * CHROME_W)
            w = 8 * CHROME_W;
        if (x + w > L->pane_x + L->pane_w)
            w = L->pane_x + L->pane_w - x;
        if (w < 4 * CHROME_W)
            w = 4 * CHROME_W;
        L->btns_x[i] = x;
        L->btns_w[i] = w;
        x += w + 8;
    }

    L->btn_x = n ? L->btns_x[0] : L->pane_x;
    L->btn_w = n ? L->btns_w[0] : 10 * CHROME_W;

    L->result_y = L->btn_y + (n ? L->btn_h + 12 : 0);

    if (demo_entry_has_log(e)) {
        L->log_x = L->pane_x;
        L->log_w = L->pane_w;
        L->log_h = dlog_rows * CELL_H + 12;
        L->log_y = L->pane_bottom - L->log_h;
    } else {
        L->log_x = L->pane_x;
        L->log_w = L->pane_w;
        L->log_h = 0;
        L->log_y = L->pane_bottom;
    }
}

/* Kernel image starts at 1 MB; kernel_end is the linker symbol after .bss. */
extern uint32_t kernel_end;

static int mmu_sample_addrs(uint32_t *out, int max)
{
    int n = 0;

    if (max < 1)
        return 0;

    out[n++] = 0;
    if (n < max) out[n++] = 0x100000u;
    if (n < max) out[n++] = (uint32_t)&kernel_end;
    if (n < max) out[n++] = heap_base();
    if (n < max && fb_phys_addr())
        out[n++] = fb_phys_addr();

    for (int i = 0; i < pci_device_count() && n < max; i++) {
        const pci_device_t *d = pci_get_device(i);
        for (int b = 0; b < 6 && n < max; b++) {
            if (!d->bar[b] || pci_bar_is_io(d->bar[b]))
                continue;
            uint32_t a = pci_bar_address(d->bar[b]);
            if (a) {
                out[n++] = a;
                break;
            }
        }
    }

    if (n < max) out[n++] = 0xF0000000u;
    return n;
}

/* Run the selected demonstration. This blocks the render loop exactly as a
 * terminal command does; the log flushes above are what keep it visibly
 * alive in the meantime. `btn` is which of the pane's action buttons was
 * clicked, left to right. */
static void demo_execute(const demo_entry_t *e, int btn)
{
    dlog_clear();
    console_set_sink(dlog_putc);

    switch (e->kind) {
    case ENTRY_SPAWN: {
        uint32_t before = task_switch_count();

        spawn_result.preempt_on = task_preempt_enabled();
        kprintf("three tasks, tight loops, no yields:\n\n");
        spawn_printers(3);

        spawn_result.switches = task_switch_count() - before;
        spawn_result.ran      = true;
        kprintf("\n\n%u context switches while they ran.\n",
                spawn_result.switches);
        break;
    }

    case ENTRY_PREEMPT:
        task_set_preempt(!task_preempt_enabled());
        kprintf("preemption is now %s.\n",
                task_preempt_enabled() ? "ON" : "OFF");
        if (!task_preempt_enabled()) {
            if (demo_worker_count() > 0)
                kprintf("workers still yield, so the desktop keeps running.\n");
            else
                kprintf("run the preemption proof to watch scheduling break.\n");
        }
        break;

    case ENTRY_BG:
        if (demo_worker_count() > 0) {
            kprintf("asking the workers to finish...\n");
            stop_background();
            kprintf("stopped. %d task%s left.\n",
                    task_count(), task_count() == 1 ? "" : "s");
        } else {
            int n = spawn_background(3);
            if (!n) {
                kprintf("workers already running.\n");
            } else {
                kprintf("started %d workers. they yield, so preemption off is safe.\n", n);
                kprintf("open Task Manager: worker Ticks climb.\n");
            }
        }
        break;

    case ENTRY_SCHED:
        if (btn == 1) {
            task_set_preempt(!task_preempt_enabled());
            kprintf("preemption is now %s.\n",
                    task_preempt_enabled() ? "ON" : "OFF");
            if (!task_preempt_enabled() && demo_worker_count() > 0)
                kprintf("workers still yield, so this window keeps updating.\n");
        } else if (demo_worker_count() > 0) {
            stop_background();
            kprintf("workers stopped.\n");
        } else {
            int n = spawn_background(3);
            if (n)
                kprintf("started %d workers.\n", n);
            else
                kprintf("workers already running.\n");
        }
        break;

    case ENTRY_RACE: {
        bool lock = e->slot == 1;

        race_result[e->slot].runs = 0;
        race_result[e->slot].ran  = true;

        kprintf("two cashiers, one till, %u deposits each, lock %s:\n",
                (uint32_t)RACE_ITERATIONS, lock ? "held" : "not used");

        for (int i = 0; i < DEMO_RACE_RUNS; i++) {
            uint32_t r = race_run(lock);

            race_result[e->slot].total[i] = r;
            race_result[e->slot].runs     = i + 1;

            kprintf("  shift %d: till %u of %u%s\n", i + 1, r,
                    (uint32_t)RACE_EXPECTED,
                    r == RACE_EXPECTED ? "" : "  <- lost deposits");
        }
        break;
    }

    case ENTRY_PREEMPTJOB: {
        desktop_hog_info_t inf;

        desktop_hog_snapshot(&inf);
        if (inf.running) {
            kprintf("hog already running.\n");
            break;
        }
        /* btn 0 = sharing OFF (freeze), btn 1 = sharing ON (type in Notepad).
         * Spawn and return — do not wait here or Notepad never gets a turn. */
        if (btn == 1)
            kprintf("sharing ON: type in Notepad while the hog runs.\n"
                    "open Task Manager: hog Ticks climbs.\n");
        else
            kprintf("sharing OFF: Notepad and the mouse freeze ~3 seconds.\n"
                    "the hog then exits by itself. mash keys if you like.\n");
        desktop_hog_start(btn == 1, DESKTOP_HOG_TICKS);
        break;
    }

    case ENTRY_FILERACE:
        kprintf("two writers, file %s, lock %s\n",
                FILE_RACE_NAME, e->slot == 1 ? "held" : "not used");
        file_race_run(e->slot == 1);
        kprintf("open %s in Notepad or File Explorer.\n", FILE_RACE_NAME);
        break;

    case ENTRY_THREADS: {
        uint32_t ticks = threads_run(e->slot != 0);

        threads_result[e->slot].ticks = ticks;
        threads_result[e->slot].ran   = true;
        break;
    }

    case ENTRY_PRODCONS:
        if (pc_live_running()) {
            pc_live_stop();
            kprintf("live producer/consumer stopped.\n");
        } else {
            pc_live_start();
            kprintf("producer and consumer running as tasks.\n");
            kprintf("this window samples the buffer each frame.\n");
        }
        pc_result.ran = true;
        break;

    case ENTRY_DEADLOCK:
        if (btn == 1) {
            deadlock_start(true);
            kprintf("both tasks take M1 then M2. they should finish.\n");
        } else if (btn == 2) {
            deadlock_info_t d;
            deadlock_snapshot(&d);
            if (d.deadlocked) {
                deadlock_kill_victim();
                kprintf("killed lock_a; lock_b can now take M1 and finish.\n");
            } else {
                deadlock_stop();
                kprintf("reset.\n");
            }
        } else {
            deadlock_start(false);
            kprintf("A takes M1 then M2; B takes M2 then M1.\n");
            kprintf("they wait for each other. this window does not.\n");
        }
        break;

    case ENTRY_MMU: {
        uint32_t addrs[8];
        int n = mmu_sample_addrs(addrs, 8);
        if (n < 1)
            break;

        if (btn == 1) {
            uint32_t a = mmu_walked ? mmu_last.virt : addrs[mmu_walk_sel % n];
            paging_walk(a, &mmu_last);
            mmu_walked = true;
            if (!mmu_last.present) {
                kprintf("walking %08x: not mapped. spawning a fault...\n", a);
                fault_spawn("page", a);
            } else {
                kprintf("walking %08x: already mapped at %08x, no fault.\n",
                        a, mmu_last.phys);
            }
            break;
        }

        paging_walk(addrs[mmu_walk_sel], &mmu_last);
        mmu_walked = true;
        kprintf("walk %08x -> dir[%u] pde=%08x\n",
                mmu_last.virt, mmu_last.dir_index, mmu_last.pde);
        if (!mmu_last.present)
            kprintf("  not present. Fault this address to raise #PF.\n");
        else if (mmu_last.large)
            kprintf("  4 MB page, phys %08x\n", mmu_last.phys);
        else
            kprintf("  pte=%08x phys %08x\n", mmu_last.pte, mmu_last.phys);
        mmu_walk_sel = (mmu_walk_sel + 1) % n;
        break;
    }

    case ENTRY_FAULT:
        fault_result[e->slot].before = task_count();
        fault_spawn(e->arg, 0);
        fault_result[e->slot].after = task_count();
        fault_result[e->slot].ran   = true;
        break;

    case ENTRY_USER:
        user_mode_demo(e->slot == 1);
        user_result[e->slot].completed = user_mode_completed();
        user_result[e->slot].ran       = true;
        break;

    case ENTRY_GPUTEST: {
        /* The same measurement as the gputest shell command: fill the whole
         * screen both ways and count timer ticks. Full screen rather than a
         * polite rectangle because that is the workload the numbers claim to
         * compare; the flashing is the demonstration running, and the scene
         * repaints as soon as the click handler returns. */
        const int rounds = 60;
        int w = (int)fb_width();
        int h = (int)fb_height();

        kprintf("filling %dx%d, %d times each way...\n", w, h, rounds);

        uint32_t start = pit_ticks();
        for (int i = 0; i < rounds; i++) {
            fb_fill_rect(0, 0, w, h, fb_rgb((uint8_t)(i * 4), 20, 60));
            fb_present();
        }
        gpu_result.cpu_ticks = pit_ticks() - start;

        start = pit_ticks();
        for (int i = 0; i < rounds; i++) {
            svga_fill_rect(0, 0, w, h, fb_rgb(20, (uint8_t)(i * 4), 60));
            svga_sync();
        }
        gpu_result.gpu_ticks = pit_ticks() - start;
        gpu_result.ran       = true;

        kprintf("cpu %u ticks, gpu %u ticks.\n",
                gpu_result.cpu_ticks, gpu_result.gpu_ticks);

        fb_mark_all_dirty();
        break;
    }

    default:
        break;
    }

    console_set_sink(term_putc);
}

/* A one-line coloured banner: the conclusion of a run, stated rather than
 * left for the viewer to infer from the log. */
static void demo_banner(const demos_layout_t *L, int y, uint32_t colour,
                        const char *text)
{
    char shown[80];

    fit_text(text, shown, (int)sizeof(shown), L->pane_w - 24);

    fb_fill_round_rect(L->pane_x, y, L->pane_w, CHROME_H + 10, 6, colour);
    fb_text_aa(L->pane_x + 12, y + 5, shown, col_white, true);
}

static void demo_hint(const demos_layout_t *L)
{
    fb_text_aa(L->pane_x, L->result_y,
               "Results appear here after a run.", col_title_off, true);
}

/* Camera-readable status block. Title is the thing the instructor should
 * be able to read from across the room.
 *
 * Written but not yet called from anywhere. Marked unused so that -Werror
 * does not fail the build on it -- deleting it would throw the work away,
 * and wiring it in blind would mean guessing which pane it belongs in. */
__attribute__((unused))
static int demo_hero(const demos_layout_t *L, int y, uint32_t colour,
                     const char *title, const char *sub)
{
    int h = CHROME_H * 3 + 18;

    fb_fill_round_rect(L->pane_x, y, L->pane_w, h, 8, colour);
    fb_text_aa(L->pane_x + 16, y + 8, title, col_white, true);
    if (sub && sub[0])
        fb_text_aa(L->pane_x + 16, y + 8 + CHROME_H + 6, sub, col_white, true);

    return y + h + 10;
}

/* An indicator lamp with a caption, for the two entries whose interesting
 * state is current rather than the outcome of a run. */
static void demo_lamp(const demos_layout_t *L, bool on, const char *caption,
                      const char *detail)
{
    int d = CHROME_H + 6;

    /* Wipe the previous caption. Start/Stop and ON/OFF labels change
     * length, and drawing the new string over the old one left a smear. */
    fb_fill_rect(L->pane_x, L->result_y, L->pane_w,
                 d + CHROME_H + 14, col_face);

    fb_fill_round_rect(L->pane_x, L->result_y, d, d, d / 2,
                       on ? col_ok : col_close);
    fb_text_aa(L->pane_x + d + 12, L->result_y + (d - CHROME_H) / 2,
               caption, col_black, true);
    fb_text_aa(L->pane_x, L->result_y + d + 10, detail, col_title_off, true);
}

static void draw_race_result(const demos_layout_t *L, int slot)
{
    if (!race_result[slot].ran) {
        demo_hint(L);
        return;
    }

    int label_w = fb_text_width("shift 8:", true) + 10;
    int value_w = fb_text_width("lost 888", true) + 12;
    int bar_x   = L->pane_x + label_w;
    int bar_w   = L->pane_w - label_w - value_w - 10;
    int bh      = CHROME_H + 10;
    int step    = bh + 10;
    int y       = L->result_y;

    if (bar_w < 40)
        bar_w = 40;

    bool all_ok = true;
    uint32_t worst_lost = 0;

    for (int i = 0; i < race_result[slot].runs; i++) {
        uint32_t total = race_result[slot].total[i];
        bool     ok    = total == (uint32_t)RACE_EXPECTED;
        uint32_t lost  = ok ? 0 : (uint32_t)RACE_EXPECTED - total;

        all_ok &= ok;
        if (lost > worst_lost)
            worst_lost = lost;

        char label[10] = "shift 1:";
        label[6] = (char)('1' + i);

        fb_text_aa(L->pane_x, y, label, col_black, true);

        int fill = (int)((uint32_t)bar_w * total / (uint32_t)RACE_EXPECTED);
        if (fill > bar_w)
            fill = bar_w;

        fb_fill_rect(bar_x, y + 2, bar_w, bh, col_head);
        fb_fill_rect(bar_x, y + 2, fill, bh,
                     ok ? col_ok : col_close);

        char v[24];
        int  n;

        if (ok) {
            n = str_append(v, 0, "till ");
            n = n + u32_to_str(total, v + n);
        } else {
            n = str_append(v, 0, "lost ");
            n = n + u32_to_str(lost, v + n);
        }

        fb_text_aa(L->pane_x + L->pane_w - fb_text_width(v, true), y, v,
                   ok ? col_ok : col_close, true);

        y += step;
    }

    y += 4;

    if (slot == 1)
        demo_banner(L, y, all_ok ? col_ok : col_close, all_ok
                    ? "Only one cashier at the till at a time."
                    : "UNEXPECTED: the lock should have made these exact.");
    else if (all_ok)
        demo_banner(L, y, col_title_off,
                    "Lucky - till matched. Timing. Run again.");
    else {
        char cap[72];
        int n = str_append(cap, 0, "Lost deposits: ");
        n = n + u32_to_str(worst_lost, cap + n);
        str_append(cap, n, ". Both cashiers read the same old total.");
        demo_banner(L, y, col_close, cap);
    }
}

static void draw_threads_result(const demos_layout_t *L)
{
    uint32_t seq = threads_result[0].ran ? threads_result[0].ticks : 0;
    uint32_t thr = threads_result[1].ran ? threads_result[1].ticks : 0;
    uint32_t worst = seq > thr ? seq : thr;

    if (!threads_result[0].ran && !threads_result[1].ran) {
        demo_hint(L);
        return;
    }

    if (worst == 0)
        worst = 1;

    int label_w = fb_text_width("overlapping", true) + 14;
    int value_w = fb_text_width("8888 ticks", true) + 12;
    int bar_x   = L->pane_x + label_w;
    int bar_w   = L->pane_w - label_w - value_w - 10;
    int y       = L->result_y;
    int bh      = CHROME_H + 4;

    if (bar_w < 40)
        bar_w = 40;

    if (threads_result[0].ran) {
        int fill = (int)((uint32_t)bar_w * seq / worst);

        fb_text_aa(L->pane_x, y, "sequential", col_black, true);
        fb_fill_rect(bar_x, y + 2, bar_w, bh, col_head);
        fb_fill_rect(bar_x, y + 2, fill, bh, col_btn_on);

        char v[24];
        int  n = u32_to_str(seq, v);
        n = str_append(v, n, " ticks");
        fb_text_aa(L->pane_x + L->pane_w - fb_text_width(v, true), y, v,
                   col_black, true);
        y += CHROME_H + 10;
    }

    if (threads_result[1].ran) {
        bool faster = threads_result[0].ran && thr < seq;
        int fill = (int)((uint32_t)bar_w * thr / worst);

        fb_text_aa(L->pane_x, y, "overlapping", col_black, true);
        fb_fill_rect(bar_x, y + 2, bar_w, bh, col_head);
        fb_fill_rect(bar_x, y + 2, fill, bh, faster ? col_ok : col_btn_on);

        char v[24];
        int  n = u32_to_str(thr, v);
        n = str_append(v, n, " ticks");
        fb_text_aa(L->pane_x + L->pane_w - fb_text_width(v, true), y, v,
                   faster ? col_ok : col_black, true);
        y += CHROME_H + 14;
    }

    if (threads_result[0].ran && threads_result[1].ran && thr < seq)
        demo_banner(L, y, col_ok,
                    "Second thread computed during the first one's wait.");
    else if (threads_result[0].ran && threads_result[1].ran)
        demo_banner(L, y, col_close,
                    "Threaded was not shorter - run both again.");
    else
        fb_text_aa(L->pane_x, y,
                   "Run sequential, then overlapping, on the same jobs.",
                   col_title_off, true);

    y += CHROME_H + 10;
    fb_text_aa(L->pane_x, y,
               "One CPU: two compute-only threads would not finish sooner.",
               col_title_off, true);
    y += CHROME_H + 6;
    fb_text_aa(L->pane_x, y,
               "This window staying usable is the same overlap.",
               col_title_off, true);
}

static void draw_desktop_hog(const demos_layout_t *L)
{
    desktop_hog_info_t hog;
    int y = L->result_y;
    char line[72];
    int n;

    desktop_hog_snapshot(&hog);

    if (hog.running) {
        demo_banner(L, y, hog.sharing ? col_ok : col_close,
                    hog.sharing
                    ? "Sharing on: type in Notepad - keys appear while the hog is still in Task Manager."
                    : "Sharing off: type in Notepad - it will freeze until the hog finishes.");
        y += CHROME_H + 16;
        n = str_append(line, 0, "hog Ticks ");
        n = n + u32_to_str(hog.hog_cpu_ticks, line + n);
        fb_text_aa(L->pane_x, y, line, col_black, true);
    } else if (hog.finished) {
        uint32_t wall = (hog.end_tick > hog.start_tick)
                        ? hog.end_tick - hog.start_tick : 0;

        demo_banner(L, y, hog.sharing ? col_ok : col_close,
                    hog.sharing
                    ? "Hog finished. Notepad should have taken keys."
                    : "Hog finished. The desktop was frozen; it is back.");
        y += CHROME_H + 16;
        n = str_append(line, 0, "ran ");
        n = n + u32_to_str(wall, line + n);
        n = str_append(line, n, " ticks then exited.");
        fb_text_aa(L->pane_x, y, line, col_black, true);
    } else {
        demo_hint(L);
        y += CHROME_H + 8;
    }

    y += CHROME_H + 14;
    fb_text_aa(L->pane_x, y,
               "Task Manager: the hog's Ticks number goes up - that is the thread using the CPU.",
               col_title_off, true);
}

static void draw_filerace_result(const demos_layout_t *L, int slot)
{
    file_race_info_t info;
    char line[72];
    int n, y = L->result_y;

    (void)slot;
    file_race_snapshot(&info);
    if (!info.ran) {
        demo_hint(L);
        return;
    }

    n = str_append(line, 0, FILE_RACE_NAME);
    n = str_append(line, n, ": ");
    n = n + u32_to_str(info.size, line + n);
    n = str_append(line, n, " bytes");
    fb_text_aa(L->pane_x, y, line, col_black, true);
    y += CHROME_H + 8;

    n = str_append(line, 0, "clean A ");
    n = n + u32_to_str((uint32_t)info.intact_a, line + n);
    n = str_append(line, n, "   clean B ");
    n = n + u32_to_str((uint32_t)info.intact_b, line + n);
    n = str_append(line, n, "   torn ");
    n = n + u32_to_str((uint32_t)info.torn, line + n);
    fb_text_aa(L->pane_x, y, line, col_black, true);
    y += CHROME_H + 12;

    if (info.locked)
        demo_banner(L, y, info.torn == 0 ? col_ok : col_close,
                    info.torn == 0
                    ? "Open till.log in Notepad - whole lines."
                    : "UNEXPECTED: the lock should have kept lines intact.");
    else
        demo_banner(L, y, info.torn > 0 ? col_close : col_title_off,
                    info.torn > 0
                    ? "Open till.log in Notepad - torn letters vs whole lines."
                    : "No tear this time - run again.");
}

static void draw_gpu_result(const demos_layout_t *L)
{
    if (!gpu_result.ran) {
        demo_hint(L);
        return;
    }

    uint32_t cpu = gpu_result.cpu_ticks;
    uint32_t gpu = gpu_result.gpu_ticks;
    uint32_t worst = cpu > gpu ? cpu : gpu;

    if (worst == 0)
        worst = 1;

    int label_w = fb_text_width("GPU", true) + 14;
    int value_w = fb_text_width("8888 ticks (88880 ms)", true) + 12;
    int bar_x   = L->pane_x + label_w;
    int bar_w   = L->pane_w - label_w - value_w - 10;
    int step    = CHROME_H + 10;
    int y       = L->result_y;

    if (bar_w < 40)
        bar_w = 40;

    static const char *names[2] = { "CPU", "GPU" };
    uint32_t ticks[2]   = { cpu, gpu };
    uint32_t colours[2] = { 0, 0 };

    colours[0] = col_title_off;
    colours[1] = col_accent;

    for (int i = 0; i < 2; i++) {
        fb_text_aa(L->pane_x, y, names[i], col_black, true);

        int fill = (int)((uint32_t)bar_w * ticks[i] / worst);
        if (fill < 2)
            fill = 2;       /* a zero-tick bar should still be visible */

        fb_fill_rect(bar_x, y + 2, bar_w, CHROME_H - 4, col_head);
        fb_fill_rect(bar_x, y + 2, fill, CHROME_H - 4, colours[i]);

        char v[40];
        int  n = u32_to_str(ticks[i], v);
        n = str_append(v, n, " ticks (");
        n = n + u32_to_str(ticks[i] * 10, v + n);
        n = str_append(v, n, " ms)");

        fb_text_aa(L->pane_x + L->pane_w - fb_text_width(v, true), y, v,
                   col_black, true);

        y += step;
    }

    y += 2;

    char caption[80];
    int  n = 0;

    if (gpu == 0) {
        /* Faster than a 100 Hz clock can resolve, so state the bound the
         * measurement supports rather than inventing a ratio. */
        n = str_append(caption, n, "Inside one timer tick - at least ");
        n = n + u32_to_str(cpu, caption + n);
        n = str_append(caption, n, "x faster.");
        demo_banner(L, y, col_ok, caption);
    } else if (cpu > gpu) {
        n = n + u32_to_str(cpu / gpu, caption + n);
        n = str_append(caption, n, "x faster than the CPU path.");
        demo_banner(L, y, col_ok, caption);
    } else {
        demo_banner(L, y, col_title_off,
                    "No faster here - an emulated adapter uses the host CPU.");
    }
}

static void draw_fault_result(const demos_layout_t *L, int slot)
{
    if (!fault_result[slot].ran) {
        demo_hint(L);
        return;
    }

    bool ok = fault_result[slot].after == fault_result[slot].before;

    char text[80];
    int  n = 0;

    if (ok) {
        n = str_append(text, n, "Task killed, kernel intact: ");
        n = n + u32_to_str((uint32_t)fault_result[slot].before, text + n);
        n = str_append(text, n, " before, ");
        n = n + u32_to_str((uint32_t)fault_result[slot].after, text + n);
        n = str_append(text, n, " after.");
    } else {
        n = str_append(text, n, "Task count did not return to normal.");
    }

    demo_banner(L, L->result_y, ok ? col_ok : col_close, text);
}

static void draw_user_result(const demos_layout_t *L, int slot)
{
    if (!user_result[slot].ran) {
        demo_hint(L);
        return;
    }

    bool completed = user_result[slot].completed;

    if (slot == 0)
        demo_banner(L, L->result_y,
                    completed ? col_close : col_ok, completed
                    ? "The hardware write went through - NOT expected."
                    : "The CPU blocked it: task killed, kernel intact.");
    else
        demo_banner(L, L->result_y,
                    completed ? col_ok : col_close, completed
                    ? "Ran unprivileged and exited cleanly via int 0x80."
                    : "Did not reach its exit syscall.");
}

/* ---- the live panels. Nothing here is cached: every frame reads the
 * scheduler, the heap, the bus and the adapter afresh, which is the same
 * decision the file manager made and for the same reason. */

static void draw_live_tasks(const demos_layout_t *L)
{
    int y    = L->btn_y;
    int step = CHROME_H + 5;

    int pid_right   = L->pane_x + fb_text_width("888", true);
    int name_x      = pid_right + 16;
    int state_x     = L->pane_x + (L->pane_w * 46) / 100;
    int ticks_right = L->pane_x + L->pane_w - 4;

    fb_text_aa(L->pane_x, y, "PID", col_title_off, true);
    fb_text_aa(name_x, y, "NAME", col_title_off, true);
    fb_text_aa(state_x, y, "STATE", col_title_off, true);
    fb_text_aa(ticks_right - fb_text_width("TICKS", true), y,
               "TICKS", col_title_off, true);

    y += step;
    fb_blend_rect(L->pane_x, y - 3, L->pane_w, 1, col_black, 40);

    for (task_t *t = task_list(); t; t = t->next) {
        if (y + step > L->pane_bottom) {
            fb_text_aa(L->pane_x, y, "...", col_title_off, true);
            break;
        }

        draw_number_right(pid_right, y, (uint32_t)t->id, col_black);

        char nm[TASK_NAME_LEN + 2];
        fit_text(t->name, nm, (int)sizeof(nm), state_x - name_x - 10);
        fb_text_aa(name_x, y, nm, col_black, true);

        uint32_t sc = col_black;
        if (t->state == TASK_RUNNING)      sc = col_ok;
        else if (t->state == TASK_BLOCKED) sc = col_title_off;
        else if (t->state == TASK_DEAD)    sc = col_close;

        fb_text_aa(state_x, y, task_state_name(t->state), sc, true);
        draw_number_right(ticks_right, y, t->ticks, col_title_on);

        y += step;
    }
}

static void draw_live_mem(const demos_layout_t *L)
{
    heap_stats_t s;
    heap_get_stats(&s);

    int y       = L->btn_y;
    int step    = CHROME_H + 6;
    int value_x = L->pane_x + fb_text_width("largest free", true) + 18;

    static const char *labels[4] = { "total", "used", "free", "largest free" };
    uint32_t kb[4]     = { s.total_bytes / 1024, s.used_bytes / 1024,
                           s.free_bytes / 1024, s.largest_free / 1024 };
    uint32_t blocks[4] = { 0, s.used_blocks, s.free_blocks, 0 };

    for (int i = 0; i < 4; i++) {
        fb_text_aa(L->pane_x, y, labels[i], col_title_off, true);

        char v[40];
        int  n = u32_to_str(kb[i], v);
        n = str_append(v, n, " KB");

        if (blocks[i]) {
            n = str_append(v, n, " in ");
            n = n + u32_to_str(blocks[i], v + n);
            n = str_append(v, n, blocks[i] == 1 ? " block" : " blocks");
        }

        fb_text_aa(value_x, y, v, col_black, true);
        y += step;
    }

    /* Usage bar. Computed in KB so the multiply cannot overflow 32 bits —
     * bytes times a pixel width already can. */
    y += 4;

    int bar_w   = L->pane_w - 8;
    uint32_t tk = s.total_bytes / 1024;
    int fill    = tk ? (int)((s.used_bytes / 1024) * (uint32_t)bar_w / tk) : 0;

    fb_fill_rect(L->pane_x, y, bar_w, CHROME_H - 2, col_head);
    fb_fill_rect(L->pane_x, y, fill, CHROME_H - 2, col_title_on);
    y += CHROME_H + 8;

    bool intact = heap_check() == 0;
    fb_text_aa(L->pane_x, y, intact ? "structure intact" : "HEAP CORRUPT",
               intact ? col_ok : col_close, true);
}

static void draw_live_pci(const demos_layout_t *L)
{
    int cell = CELL_H > CHROME_H ? CELL_H : CHROME_H;
    int y    = L->btn_y;
    int step = cell + 5;

    int addr_x    = L->pane_x;
    int id_x      = addr_x + 8 * CELL_W + 12;
    int class_x   = id_x + 10 * CELL_W + 12;
    int irq_right = L->pane_x + L->pane_w - 4;
    int class_w   = irq_right - fb_text_width("IRQ", true) - 14 - class_x;

    fb_text_aa(addr_x, y, "ADDRESS", col_title_off, true);
    fb_text_aa(id_x, y, "ID", col_title_off, true);
    fb_text_aa(class_x, y, "CLASS", col_title_off, true);
    fb_text_aa(irq_right - fb_text_width("IRQ", true), y,
               "IRQ", col_title_off, true);

    y += step;
    fb_blend_rect(L->pane_x, y - 3, L->pane_w, 1, col_black, 40);

    int count = pci_device_count();

    for (int i = 0; i < count; i++) {
        if (y + step > L->pane_bottom) {
            char more[16];
            int  n = str_append(more, 0, "+");
            n = n + u32_to_str((uint32_t)(count - i), more + n);
            n = str_append(more, n, " more");
            fb_text_aa(L->pane_x, y, more, col_title_off, true);
            break;
        }

        const pci_device_t *d = pci_get_device(i);

        char addr[12];
        int  n = hex_append(addr, 0, d->bus, 2);
        addr[n++] = ':';
        n = hex_append(addr, n, d->device, 2);
        addr[n++] = '.';
        addr[n++] = (char)('0' + (d->function & 7));
        addr[n]   = '\0';
        fb_text_aa(addr_x, y, addr, col_black, false);

        char id[12];
        n = hex_append(id, 0, d->vendor_id, 4);
        id[n++] = ':';
        n = hex_append(id, n, d->device_id, 4);
        id[n]   = '\0';
        fb_text_aa(id_x, y, id, col_black, false);

        char cls[40];
        fit_text(pci_class_name(d->class_code, d->subclass), cls,
                 (int)sizeof(cls), class_w);
        fb_text_aa(class_x, y, cls, col_black, true);

        if (d->irq_line != 0xFF)
            draw_number_right(irq_right, y, d->irq_line, col_black);
        else
            fb_text_aa(irq_right - fb_text_width("-", true), y, "-",
                       col_title_off, true);

        y += step;
    }
}

static void draw_live_gpu(const demos_layout_t *L)
{
    int y    = L->btn_y;
    int step = CHROME_H + 6;

    char name[64];
    fit_text(svga_name(), name, (int)sizeof(name), L->pane_w);
    fb_text_aa(L->pane_x, y, name, col_title_on, true);
    y += step + 4;

    if (!svga_available()) {
        fb_text_aa(L->pane_x, y,
                   "No accelerated adapter found. Every pixel on",
                   col_black, true);
        fb_text_aa(L->pane_x, y + step,
                   "this screen is written by the CPU.", col_black, true);
        return;
    }

    static const char *labels[4] = {
        "accelerated fill", "accelerated copy", "3d capability bit",
        "fifo commands"
    };

    int value_x = L->pane_x + fb_text_width("3d capability bit", true) + 18;

    for (int i = 0; i < 4; i++) {
        fb_text_aa(L->pane_x, y, labels[i], col_title_off, true);

        if (i == 3) {
            char v[12];
            u32_to_str(svga_command_count(), v);
            fb_text_aa(value_x, y, v, col_black, true);
        } else {
            bool yes = i == 0 ? svga_can_fill()
                     : i == 1 ? svga_can_copy()
                              : svga_has_3d();
            fb_text_aa(value_x, y, yes ? "yes" : "no",
                       yes ? col_ok : col_title_off, true);
        }

        y += step;
    }

    /* Honest reporting: the capability bit alone does not make a pipeline.
     * On this guest the extended FIFO reports a 3D hardware version of zero
     * and the 3D driver declines, so no claim beyond 2D is made here. */
    if (svga_has_3d() && svga_fifo_3d_hwversion() == 0) {
        fb_text_aa(L->pane_x, y, "3d pipeline unavailable: the adapter",
                   col_title_off, true);
        fb_text_aa(L->pane_x, y + step, "reports hardware version 0.",
                   col_title_off, true);
        y += 2 * step;
    }

    y += 4;
    fb_text_aa(L->pane_x, y, "The command counter above ticks while the",
               col_title_off, true);
    fb_text_aa(L->pane_x, y + step, "desktop draws - that traffic is live.",
               col_title_off, true);
}

/* The status line along the bottom: the numbers a demonstration changes,
 * visible without switching to a live view. */
static int demo_strip_pair(int x, int y, const char *label, const char *value,
                           uint32_t vcol)
{
    fb_text_aa(x, y, label, col_title_off, true);
    x += fb_text_width(label, true) + 6;
    fb_text_aa(x, y, value, vcol, true);

    return x + fb_text_width(value, true) + 20;
}

static void draw_demo_strip(const demos_layout_t *L)
{
    fb_blend_rect(L->cx, L->strip_y - 5, L->cw, 1, col_black, 40);

    int x = L->cx + 2;
    int y = L->strip_y;

    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;

    char up[16];
    int  n = u32_to_str(secs / 3600, up);
    up[n++] = ':';
    up[n++] = (char)('0' + (secs / 600) % 6);
    up[n++] = (char)('0' + (secs / 60) % 10);
    up[n++] = ':';
    up[n++] = (char)('0' + (secs / 10) % 6);
    up[n++] = (char)('0' + secs % 10);
    up[n]   = '\0';

    char tasks[12], switches[12];
    u32_to_str((uint32_t)task_count(), tasks);
    u32_to_str(task_switch_count(), switches);

    x = demo_strip_pair(x, y, "up", up, col_black);
    x = demo_strip_pair(x, y, "tasks", tasks, col_black);
    x = demo_strip_pair(x, y, "switches", switches, col_black);

    bool pre = task_preempt_enabled();
    demo_strip_pair(x, y, "preempt", pre ? "on" : "OFF",
                    pre ? col_ok : col_close);
}

static uint32_t task_colour(uint32_t id)
{
    /* Idle ticks are a pale grey so the timeline is honest: at rest the
     * bar is almost entirely this, not the kernel's colour. */
    if (task_idle_id() && id == (uint32_t)task_idle_id())
        return fb_rgb(198, 202, 210);

    switch (id % 6) {
    case 0: return col_btn_on;
    case 1: return col_ok;
    case 2: return col_folder;
    case 3: return fb_rgb(160, 100, 180);
    case 4: return fb_rgb(70, 150, 170);
    default: return col_close;
    }
}

static void draw_tick_strip(int x, int y, int w, int h)
{
    uint32_t ids[SCHED_TRACE_LEN];
    uint32_t n = task_sched_trace(ids, SCHED_TRACE_LEN);

    fb_fill_round_rect(x, y, w, h, 4, col_list_bg);
    if (n == 0)
        return;

    int cell = w / (int)n;
    if (cell < 2)
        cell = 2;

    for (uint32_t i = 0; i < n; i++) {
        int cx = x + (int)i * cell;
        int cw = cell - 1;
        if (cx + cw > x + w)
            break;
        fb_fill_rect(cx, y + 2, cw, h - 4, task_colour(ids[i]));
    }
}

static void draw_sched_viz(const demos_layout_t *L)
{
    int y = L->result_y;
    int w = L->pane_w;
    int h = CHROME_H + 10;

    fb_text_aa(L->pane_x, y, "recent ticks (left = older, grey = idle)",
               col_title_off, true);
    y += CHROME_H + 6;

    draw_tick_strip(L->pane_x, y, w, h);

    y += h + 10;
    fb_text_aa(L->pane_x, y, "CPU time (ticks)", col_title_off, true);
    y += CHROME_H + 6;

    uint32_t max_ticks = 1;
    for (task_t *t = task_list(); t; t = t->next)
        if (t->ticks > max_ticks)
            max_ticks = t->ticks;

    int step = CHROME_H + 8;
    int bar_x = L->pane_x + 90;
    int bar_w = L->pane_x + w - bar_x - 50;
    if (bar_w < 40)
        bar_w = 40;

    for (task_t *t = task_list(); t; t = t->next) {
        if (y + step > L->pane_bottom)
            break;

        char nm[TASK_NAME_LEN + 2];
        fit_text(t->name, nm, (int)sizeof(nm), 86);
        fb_text_aa(L->pane_x, y, nm, col_black, true);

        int fill = (int)((t->ticks * (uint32_t)bar_w) / max_ticks);
        if (fill < 2 && t->ticks > 0)
            fill = 2;

        fb_fill_rect(bar_x, y + 2, bar_w, CHROME_H, col_list_bg);
        fb_fill_rect(bar_x, y + 2, fill, CHROME_H, task_colour((uint32_t)t->id));
        draw_number_right(L->pane_x + w - 4, y, t->ticks, col_title_on);
        y += step;
    }
}

typedef enum {
    REG_UNMAPPED, REG_NULL, REG_LOW, REG_KERNEL, REG_HEAP, REG_RAM, REG_FB, REG_PCI
} mem_reg_t;

static mem_reg_t classify_addr(uint32_t a)
{
    extern uint32_t kernel_end;
    uint32_t kend = (uint32_t)&kernel_end;
    uint32_t h0 = heap_base();
    uint32_t h1 = h0 + heap_size_bytes();
    uint32_t f0 = fb_phys_addr();
    uint32_t f1 = f0 + fb_nbytes();

    if (a < 0x1000)
        return REG_NULL;

    if (f0 && a >= f0 && a < f1)
        return REG_FB;

    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_get_device(i);
        for (int b = 0; b < 6; b++) {
            if (!d->bar[b] || pci_bar_is_io(d->bar[b]))
                continue;
            uint32_t bar = pci_bar_address(d->bar[b]);
            if (bar && a >= bar && a < bar + PAGING_LARGE_SIZE)
                return REG_PCI;
        }
    }

    if (a >= 0x100000 && a < kend)
        return REG_KERNEL;
    if (a >= h0 && a < h1)
        return REG_HEAP;
    if (a < 0x100000)
        return REG_LOW;

    page_walk_t w;
    paging_walk(a, &w);
    if (!w.present)
        return REG_UNMAPPED;
    return REG_RAM;
}

static uint32_t reg_colour(mem_reg_t r)
{
    switch (r) {
    case REG_NULL:     return col_close;
    case REG_LOW:      return fb_rgb(180, 180, 190);
    case REG_KERNEL:   return col_btn_on;
    case REG_HEAP:     return col_ok;
    case REG_RAM:      return col_accent;
    case REG_FB:       return col_folder;
    case REG_PCI:      return fb_rgb(160, 100, 180);
    default:           return col_list_bg;
    }
}

static const char *reg_name(mem_reg_t r)
{
    switch (r) {
    case REG_NULL:     return "page 0 (unmapped)";
    case REG_LOW:      return "low memory";
    case REG_KERNEL:   return "kernel image";
    case REG_HEAP:     return "heap";
    case REG_RAM:      return "RAM";
    case REG_FB:       return "framebuffer";
    case REG_PCI:      return "PCI MMIO";
    default:           return "unmapped";
    }
}

static void draw_mmu_viz(const demos_layout_t *L)
{
    int y = L->result_y;
    int w = L->pane_w;
    int bar_h = CHROME_H + 8;

    fb_text_aa(L->pane_x, y, "low 256 MB (left) and high memory (right)",
               col_title_off, true);
    y += CHROME_H + 4;
    fb_fill_round_rect(L->pane_x, y, w, bar_h, 4, col_list_bg);

    /* The first 256 MB is where the kernel, the heap and ordinary RAM
     * actually live; drawn at 4 MB per slice so those regions are wide
     * enough to see. The remaining quarter of the bar is 256 MB..4 GB,
     * which is where the framebuffer and PCI MMIO sit. */
    int split = (w * 3) / 4;
    int slices = 64;    /* 256 MB / 4 MB */

    mem_reg_t prev = classify_addr(0);
    int start_i = 0;
    for (int i = 1; i <= slices; i++) {
        mem_reg_t r = (i < slices)
                    ? classify_addr((uint32_t)i * PAGING_LARGE_SIZE)
                    : (mem_reg_t)255;
        if (r == prev && i < slices)
            continue;
        int x0 = L->pane_x + (start_i * split) / slices;
        int x1 = L->pane_x + (i * split) / slices;
        if (x1 > x0)
            fb_fill_rect(x0, y + 2, x1 - x0, bar_h - 4, reg_colour(prev));
        start_i = i;
        prev = r;
    }

    prev = classify_addr(0x10000000u);
    start_i = 0;
    int high_w = w - split;
    for (int i = 1; i <= 64; i++) {
        uint32_t addr = 0x10000000u + (uint32_t)i * ((0xF0000000u - 0x10000000u) / 64);
        mem_reg_t r = (i < 64) ? classify_addr(addr) : (mem_reg_t)255;
        if (r == prev && i < 64)
            continue;
        int x0 = L->pane_x + split + (start_i * high_w) / 64;
        int x1 = L->pane_x + split + (i * high_w) / 64;
        if (x1 > x0)
            fb_fill_rect(x0, y + 2, x1 - x0, bar_h - 4, reg_colour(prev));
        start_i = i;
        prev = r;
    }
    fb_fill_rect(L->pane_x + split, y + 1, 1, bar_h - 2, col_title_off);

    y += bar_h + 8;

    static const mem_reg_t legend[] = {
        REG_NULL, REG_KERNEL, REG_HEAP, REG_RAM, REG_FB, REG_PCI, REG_UNMAPPED
    };
    int lx = L->pane_x;
    for (unsigned i = 0; i < sizeof(legend) / sizeof(legend[0]); i++) {
        fb_fill_rect(lx, y + 3, 10, 10, reg_colour(legend[i]));
        const char *nm = (legend[i] == REG_UNMAPPED) ? "unmapped" : reg_name(legend[i]);
        int tw = fb_text_width(nm, true);
        fb_text_aa(lx + 14, y, nm, col_title_off, true);
        lx += 24 + tw;
        if (lx > L->pane_x + w - 80) {
            lx = L->pane_x;
            y += CHROME_H + 6;
        }
    }

    y += CHROME_H + 12;

    if (!mmu_walked) {
        fb_text_aa(L->pane_x, y,
                   "Walk next address steps through kernel, heap, framebuffer, then an unmapped one.",
                   col_title_off, true);
        return;
    }

    page_walk_t wlk = mmu_last;
    char line[80];
    int n = str_append(line, 0, "virtual ");
    n = hex_append(line, n, wlk.virt, 8);
    n = str_append(line, n, "  ");
    n = str_append(line, n, reg_name(classify_addr(wlk.virt)));
    fb_text_aa(L->pane_x, y, line, col_black, true);
    y += CHROME_H + 10;

    int box = (w - 24) / 3;
    if (box > 200) box = 200;
    int bh = CHROME_H * 3 + 16;
    int bx = L->pane_x;
    uint32_t bc[3] = {
        col_btn_on,
        wlk.present ? col_ok : col_close,
        wlk.present ? col_accent : col_title_off
    };
    const char *bt[3] = { "directory", wlk.large ? "4 MB page" : "page table", "frame" };

    for (int i = 0; i < 3; i++) {
        fb_fill_round_rect(bx, y, box, bh, 6, bc[i]);
        fb_text_aa(bx + 10, y + 6, bt[i], col_white, true);
        if (i == 0) {
            n = str_append(line, 0, "PDE ");
            hex_append(line, n, wlk.pde, 8);
        } else if (i == 1 && !wlk.large) {
            n = str_append(line, 0, "PTE ");
            hex_append(line, n, wlk.pte, 8);
        } else if (i == 1) {
            str_append(line, 0, "no table");
        } else if (wlk.present) {
            n = str_append(line, 0, "phys ");
            hex_append(line, n, wlk.phys, 8);
        } else {
            str_append(line, 0, "unmapped");
        }
        fb_text_aa(bx + 10, y + 6 + CHROME_H + 4, line, col_white, true);
        if (i < 2)
            fb_fill_rect(bx + box + 2, y + bh / 2, 6, 2, col_title_off);
        bx += box + 12;
    }

    y += bh + 10;
    if (!wlk.present)
        fb_text_aa(L->pane_x, y,
                   "Not present. Fault this address to raise a page fault in a spawned task.",
                   col_close, true);
}

static const char *pc_state_name(pc_role_state_t s)
{
    switch (s) {
    case PC_RUNNING:    return "running";
    case PC_WAIT_FULL:  return "blocked (full)";
    case PC_WAIT_EMPTY: return "blocked (empty)";
    case PC_DONE:       return "done";
    default:            return "idle";
    }
}

static void draw_pc_viz(const demos_layout_t *L)
{
    pc_live_info_t info;
    pc_live_snapshot(&info);

    int y = L->result_y;
    int area = L->log_y - y;
    if (area > 8)
        fb_fill_rect(L->pane_x, y, L->pane_w, area, col_face);

    fb_text_aa(L->pane_x, y,
               "ring buffer  (in = next write,  out = next read)",
               col_title_off, true);
    y += CHROME_H + 8;

    int gap = 10;
    int slot_w = (L->pane_w - (PC_BUFFER_SLOTS - 1) * gap) / PC_BUFFER_SLOTS;
    if (slot_w > 120)
        slot_w = 120;
    int tag_h = CHROME_H + 6;
    int slot_h = CHROME_H * 3 + 18;
    int slots_y = y + tag_h + 4;

    for (int i = 0; i < PC_BUFFER_SLOTS; i++) {
        int x = L->pane_x + i * (slot_w + gap);
        bool filled = info.slot[i] != 0;
        bool is_in  = (info.head == i);
        bool is_out = (info.tail == i);

        if (is_in || is_out) {
            const char *tag = (is_in && is_out) ? "in+out" : (is_in ? "in" : "out");
            uint32_t tc = (is_in && is_out) ? col_close
                        : (is_in ? col_btn_on : col_folder);
            int tw = fb_text_width(tag, true) + 14;
            int tx = x + (slot_w - tw) / 2;
            if (tx < x)
                tx = x;
            fb_fill_round_rect(tx, y, tw, tag_h, 4, tc);
            fb_text_aa(tx + 7, y + (tag_h - CHROME_H) / 2, tag, col_white, true);
        }

        fb_fill_round_rect(x, slots_y, slot_w, slot_h, 6,
                           filled ? col_ok : col_list_bg);
        if (is_in)
            fb_rect(x, slots_y, slot_w, slot_h, col_btn_on);
        if (is_out)
            fb_rect(x + 2, slots_y + 2, slot_w - 4, slot_h - 4, col_folder);

        char idx[4];
        idx[0] = '#';
        idx[1] = (char)('0' + i);
        idx[2] = '\0';
        fb_text_aa(x + 8, slots_y + 6, idx,
                   filled ? col_white : col_title_off, true);

        if (filled) {
            char v[12];
            u32_to_str(info.slot[i], v);
            int vw = fb_text_width(v, true);
            fb_text_aa(x + (slot_w - vw) / 2,
                       slots_y + 6 + CHROME_H + 6, v, col_white, true);
        } else {
            int ew = fb_text_width("empty", true);
            fb_text_aa(x + (slot_w - ew) / 2,
                       slots_y + 6 + CHROME_H + 6, "empty", col_title_off, true);
        }
    }

    y = slots_y + slot_h + 14;

    int pw = (L->pane_w - 12) / 2;
    int ph = CHROME_H * 2 + 14;
    char num[16];
    int n;

    fb_fill_round_rect(L->pane_x, y, pw, ph, 6, col_btn_on);
    fb_text_aa(L->pane_x + 10, y + 6, "sem free  (empty slots)", col_white, true);
    n = u32_to_str(info.free_count < 0 ? 0 : (uint32_t)info.free_count, num);
    n = str_append(num, n, " / ");
    n = n + u32_to_str((uint32_t)PC_BUFFER_SLOTS, num + n);
    fb_text_aa(L->pane_x + 10, y + 6 + CHROME_H + 2, num, col_white, true);

    fb_fill_round_rect(L->pane_x + pw + 12, y, pw, ph, 6, col_folder);
    fb_text_aa(L->pane_x + pw + 22, y + 6, "sem used  (items ready)", col_white, true);
    n = u32_to_str(info.used_count < 0 ? 0 : (uint32_t)info.used_count, num);
    n = str_append(num, n, " / ");
    n = n + u32_to_str((uint32_t)PC_BUFFER_SLOTS, num + n);
    fb_text_aa(L->pane_x + pw + 22, y + 6 + CHROME_H + 2, num, col_white, true);

    y += ph + 10;

    n = str_append(num, 0, "occupancy  ");
    n = n + u32_to_str((uint32_t)info.occupancy, num + n);
    n = str_append(num, n, " / ");
    n = n + u32_to_str((uint32_t)PC_BUFFER_SLOTS, num + n);
    if (info.occupancy == 0)
        n = str_append(num, n, "   empty");
    else if (info.occupancy == PC_BUFFER_SLOTS)
        n = str_append(num, n, "   FULL");
    fb_text_aa(L->pane_x, y, num, col_black, true);
    y += CHROME_H + 8;

    char line[80];
    n = str_append(line, 0, "producer  ");
    n = str_append(line, n, pc_state_name(info.producer));
    n = str_append(line, n, "   produced ");
    n = n + u32_to_str((uint32_t)info.produced, line + n);
    fb_text_aa(L->pane_x, y, line,
               info.producer == PC_WAIT_FULL ? col_close : col_black, true);
    y += CHROME_H + 6;

    n = str_append(line, 0, "consumer  ");
    n = str_append(line, n, pc_state_name(info.consumer));
    n = str_append(line, n, "   consumed ");
    n = n + u32_to_str((uint32_t)info.consumed, line + n);
    fb_text_aa(L->pane_x, y, line,
               info.consumer == PC_WAIT_EMPTY ? col_close : col_black, true);

    if (info.active && info.producer == PC_DONE && info.consumer == PC_DONE)
        pc_result.ok = true;
}

static void draw_lock_box(int x, int y, int w, int h, const char *title,
                          int owner, bool wanted)
{
    uint32_t fill = owner ? col_ok : (wanted ? col_folder : col_list_bg);
    fb_fill_round_rect(x, y, w, h, 6, fill);
    fb_text_aa(x + 10, y + 6, title, owner || wanted ? col_white : col_black, true);

    char line[24];
    if (owner) {
        int n = str_append(line, 0, "held by ");
        n = n + u32_to_str((uint32_t)owner, line + n);
        fb_text_aa(x + 10, y + 6 + CHROME_H + 2, line, col_white, true);
    } else {
        fb_text_aa(x + 10, y + 6 + CHROME_H + 2,
                   wanted ? "wanted" : "free",
                   wanted ? col_white : col_title_off, true);
    }
}

static void draw_deadlock_viz(const demos_layout_t *L)
{
    deadlock_info_t d;
    deadlock_snapshot(&d);

    int y = L->result_y;
    int w = (L->pane_w - 12) / 2;
    int h = CHROME_H * 3 + 12;

    fb_fill_round_rect(L->pane_x, y, w, h, 6, col_list_bg);
    fb_fill_round_rect(L->pane_x + w + 12, y, w, h, 6, col_list_bg);

    char line[48];
    int n = str_append(line, 0, "task A");
    if (d.a_id) {
        n = str_append(line, n, "  id ");
        n = n + u32_to_str((uint32_t)d.a_id, line + n);
    }
    fb_text_aa(L->pane_x + 10, y + 6, line, col_black, true);

    n = str_append(line, 0, d.a_holds_m1 ? "holds M1" : (d.a_wants_m1 ? "wants M1" : "idle"));
    if (d.a_holds_m2 || d.a_wants_m2) {
        n = str_append(line, n, d.a_holds_m2 ? "  holds M2" : "  wants M2");
    }
    fb_text_aa(L->pane_x + 10, y + 6 + CHROME_H + 4, line,
               d.a_wants_m2 || d.a_wants_m1 ? col_close : col_ok, true);

    n = str_append(line, 0, "task B");
    if (d.b_id) {
        n = str_append(line, n, "  id ");
        n = n + u32_to_str((uint32_t)d.b_id, line + n);
    }
    fb_text_aa(L->pane_x + w + 22, y + 6, line, col_black, true);

    n = str_append(line, 0, d.b_holds_m2 ? "holds M2" : (d.b_wants_m2 ? "wants M2" : "idle"));
    if (d.b_holds_m1 || d.b_wants_m1) {
        n = str_append(line, n, d.b_holds_m1 ? "  holds M1" : "  wants M1");
    }
    fb_text_aa(L->pane_x + w + 22, y + 6 + CHROME_H + 4, line,
               d.b_wants_m2 || d.b_wants_m1 ? col_close : col_ok, true);

    y += h + 12;
    int lw = (L->pane_w - 12) / 2;
    int lh = CHROME_H * 2 + 14;
    draw_lock_box(L->pane_x, y, lw, lh, "mutex M1", d.m1_owner,
                  d.a_wants_m1 || d.b_wants_m1);
    draw_lock_box(L->pane_x + lw + 12, y, lw, lh, "mutex M2", d.m2_owner,
                  d.a_wants_m2 || d.b_wants_m2);

    y += lh + 12;
    if (d.deadlocked)
        demo_banner(L, y, col_close,
                    "Circular wait. The rest of the kernel is still running.");
    else if (d.finished && d.ordered)
        demo_banner(L, y, col_ok,
                    "Ordered acquisition: both tasks finished. No deadlock.");
    else if (d.finished)
        demo_banner(L, y, col_ok, "Both tasks finished.");
    else if (!d.active)
        fb_text_aa(L->pane_x, y,
                   "Trigger a deadlock, or run the ordered-lock fix.",
                   col_title_off, true);
}

static void draw_demos(const window_t *win, bool active)
{
    demos_layout_t L;
    demos_layout(win, &L);

    if (demo_scroll > DEMO_ENTRY_COUNT - L.rows)
        demo_scroll = DEMO_ENTRY_COUNT - L.rows;
    if (demo_scroll < 0)
        demo_scroll = 0;

    /* Sidebar. */
    fb_fill_rect(L.list_x, L.list_y, L.list_w, L.rows * L.row_h, col_list_bg);

    for (int r = 0; r < L.rows; r++) {
        int idx = demo_scroll + r;
        if (idx >= DEMO_ENTRY_COUNT)
            break;

        const demo_entry_t *e = &demo_entries[idx];
        int y  = L.list_y + r * L.row_h;
        int ty = y + (L.row_h - CHROME_H) / 2;

        if (e->kind == ENTRY_HEADER) {
            fb_text_aa(L.list_x + 10, ty, e->label, col_title_off, true);
            fb_blend_rect(L.list_x + 8, y + L.row_h - 3, L.list_w - 16, 1,
                          col_black, 40);
            continue;
        }

        bool sel = idx == demo_sel;

        if (sel)
            fb_fill_rect(L.list_x, y, L.list_w, L.row_h,
                         active ? col_list_sel : col_list_sel_off);

        fb_text_aa(L.list_x + 24, ty, e->label,
                   sel && active ? col_white : col_black, true);
    }

    /* Scroll affordance: a darkened edge where more rows are hiding. */
    if (demo_scroll > 0)
        fb_blend_rect(L.list_x, L.list_y, L.list_w, 3, col_black, 60);
    if (demo_scroll + L.rows < DEMO_ENTRY_COUNT)
        fb_blend_rect(L.list_x, L.list_y + L.rows * L.row_h - 3, L.list_w, 3,
                      col_black, 60);

    /* Pane: title, description, then whatever the entry kind calls for. */
    const demo_entry_t *e = &demo_entries[demo_sel];

    fb_text_aa(L.pane_x, L.pane_y, e->label, col_title_on, true);
    fb_blend_rect(L.pane_x, L.pane_y + CHROME_H + 6, L.pane_w, 1,
                  col_black, 40);

    char fitted[64];

    for (int i = 0; i < demo_desc_lines(e); i++) {
        fit_text(e->desc[i], fitted, (int)sizeof(fitted), L.pane_w);
        fb_text_aa(L.pane_x, L.desc_y + i * (CHROME_H + 4), fitted,
                   col_black, true);
    }

    if (demo_entry_runnable(e)) {
        bool enabled = demo_entry_enabled(e);
        const char *labels[DEMO_MAX_BTNS];
        int nb = demo_fill_buttons(e, labels);

        for (int i = 0; i < nb && i < L.n_btns; i++) {
            fb_fill_round_rect(L.btns_x[i], L.btn_y, L.btns_w[i], L.btn_h, 7,
                               enabled ? col_btn_on : col_title_off);
            int tw = fb_text_width(labels[i], true);
            fb_text_aa(L.btns_x[i] + (L.btns_w[i] - tw) / 2,
                       L.btn_y + (L.btn_h - CHROME_H) / 2, labels[i],
                       col_white, true);
        }

        if (!enabled)
            fb_text_aa(L.btn_x + L.btn_w + 12,
                       L.btn_y + (L.btn_h - CHROME_H) / 2,
                       "needs the SVGA adapter - run under VMware",
                       col_title_off, true);

        switch (e->kind) {
        case ENTRY_SPAWN:
            if (!spawn_result.ran) {
                demo_hint(&L);
            } else {
                char text[64];
                int  n = 0;

                if (spawn_result.preempt_on) {
                    n = str_append(text, n, "Interleaved by the timer - ");
                    n = n + u32_to_str(spawn_result.switches, text + n);
                    n = str_append(text, n, " forced switches.");
                    demo_banner(&L, L.result_y, col_ok, text);
                } else {
                    demo_banner(&L, L.result_y, col_title_off,
                                "Preemption was OFF - each ran to completion.");
                }
            }
            break;

        case ENTRY_PREEMPT:
            demo_lamp(&L, task_preempt_enabled(),
                      task_preempt_enabled() ? "Preemption is ON"
                                             : "Preemption is OFF",
                      task_preempt_enabled()
                      ? "The timer forces a switch every few ticks."
                      : "A running task now keeps the CPU until it exits.");
            break;

        case ENTRY_BG: {
            int workers = demo_worker_count();
            char cap[48], detail[64];
            int  n = 0;

            if (workers > 0) {
                n = n + u32_to_str((uint32_t)workers, cap + n);
                n = str_append(cap, n, workers == 1 ? " worker is spinning"
                                                    : " workers are spinning");
            } else {
                n = str_append(cap, n, "No workers running");
            }

            if (workers > 0) {
                /* Sum the four slots' loop counters and print the total
                 * here, in this panel, rather than sending the viewer to
                 * Task Manager to see a number move. A caption that says
                 * "workers are spinning" and then sits still is
                 * indistinguishable from one that is lying; a total that
                 * climbs every redraw is not. Kept short: 25 characters at
                 * the widest, well under the 47-character caption this
                 * panel already draws safely elsewhere. */
                uint32_t total = background_counter(0) + background_counter(1)
                               + background_counter(2) + background_counter(3);
                int dn = str_append(detail, 0, "loops so far: ");
                u32_to_str(total, detail + dn);
            } else {
                str_append(detail, 0, "Start them, then watch the number climb.");
            }

            demo_lamp(&L, workers > 0, cap, detail);
            break;
        }

        case ENTRY_RACE:
            draw_race_result(&L, e->slot);
            break;

        case ENTRY_PREEMPTJOB:
            draw_desktop_hog(&L);
            break;

        case ENTRY_FILERACE:
            draw_filerace_result(&L, e->slot);
            break;

        case ENTRY_THREADS:
            draw_threads_result(&L);
            break;

        case ENTRY_PRODCONS:
            draw_pc_viz(&L);
            break;

        case ENTRY_SCHED:
            draw_sched_viz(&L);
            break;

        case ENTRY_DEADLOCK:
            draw_deadlock_viz(&L);
            break;

        case ENTRY_MMU:
            draw_mmu_viz(&L);
            break;

        case ENTRY_FAULT:
            draw_fault_result(&L, e->slot);
            break;

        case ENTRY_USER:
            draw_user_result(&L, e->slot);
            break;

        case ENTRY_GPUTEST:
            draw_gpu_result(&L);
            break;

        default:
            break;
        }

        /* Captured output, in the terminal's colours so it reads as what it
         * is. Drawn after the results so an over-tall result cannot bleed
         * into it. */
        if (L.log_h > 0) {
            fb_fill_round_rect(L.log_x, L.log_y, L.log_w, L.log_h, 5, col_term_bg);

            for (int row = 0; row < dlog_rows; row++)
                for (int col = 0; col < dlog_cols; col++) {
                    char c = dlog[row][col];
                    if (c != ' ')
                        fb_char_aa(L.log_x + 8 + col * CELL_W,
                                   L.log_y + 6 + row * CELL_H,
                                   c, col_term_fg, false);
                }
        }
    } else {
        switch (e->kind) {
        case ENTRY_LIVE_TASKS: draw_live_tasks(&L); break;
        case ENTRY_LIVE_MEM:   draw_live_mem(&L);   break;
        case ENTRY_LIVE_PCI:   draw_live_pci(&L);   break;
        case ENTRY_LIVE_GPU:   draw_live_gpu(&L);   break;
        default: break;
        }
    }

    draw_demo_strip(&L);
}

/* Returns true if the click did something demo-specific; the caller still
 * raises and focuses the window either way. */
static bool demos_click(const window_t *win, int mx, int my)
{
    demos_layout_t L;
    demos_layout(win, &L);

    if (in_rect(mx, my, L.list_x, L.list_y, L.list_w, L.rows * L.row_h)) {
        int idx = demo_scroll + (my - L.list_y) / L.row_h;

        if (idx >= DEMO_ENTRY_COUNT || demo_entries[idx].kind == ENTRY_HEADER)
            return false;

        demo_sel = idx;
        return true;
    }

    const demo_entry_t *e = &demo_entries[demo_sel];

    if (demo_entry_runnable(e) && demo_entry_enabled(e)) {
        for (int i = 0; i < L.n_btns; i++) {
            if (in_rect(mx, my, L.btns_x[i], L.btn_y, L.btns_w[i], L.btn_h)) {
                demo_execute(e, i);
                return true;
            }
        }
    }

    return false;
}

/* The taskbar's geometry, in one place. The Start menu already had a bug of
 * exactly this shape -- drawn from one set of expressions and clicked with
 * another -- and adding a pinned button to a bar whose layout was written out
 * twice would have been asking for it again. */
typedef struct {
    int bar_y, start_w, search_x, search_w, pin_x, pin_w, btn_x, btn_w, clock_w;
} taskbar_layout_t;

static void taskbar_layout(taskbar_layout_t *T)
{
    T->bar_y    = SCR_H - TASKBAR_H;
    T->start_w  = TASKBAR_H + 14;
    T->search_x = T->start_w + 6;
    T->search_w = 300;

    if (T->search_w > SCR_W / 4)
        T->search_w = SCR_W / 4;

    T->pin_x = T->search_x + T->search_w + 8;
    T->pin_w = 200;

    T->btn_x   = T->pin_x + T->pin_w + 2;
    T->btn_w   = 190;
    T->clock_w = 130;
}

/* The window a taskbar button stands for, or -1. File Explorer is pinned and
 * has its own button, so it is not also given one here. Kernel Lab uses the
 * same labelled-button path as Task Manager and Notepad. */
static int taskbar_button_window(const taskbar_layout_t *T, int mx, int my)
{
    int bx = T->btn_x;

    for (int i = 0; i < window_count; i++) {
        if (windows[i].kind == WIN_FILES)
            continue;
        if (bx + T->btn_w > SCR_W - T->clock_w)
            break;
        if (in_rect(mx, my, bx, T->bar_y, T->btn_w, TASKBAR_H))
            return i;

        bx += T->btn_w + 2;
    }

    return -1;
}

static int window_of_kind(window_kind_t kind)
{
    for (int i = 0; i < window_count; i++)
        if (windows[i].kind == kind)
            return i;
    return -1;
}

static void draw_taskbar(void)
{
    taskbar_layout_t T;
    taskbar_layout(&T);

    int bar_y = T.bar_y;
    int lh    = fb_font_height(true);
    int text_y = bar_y + (TASKBAR_H - lh) / 2;

    fb_fill_rect(0, bar_y, SCR_W, TASKBAR_H, col_bar);
    fb_blend_rect(0, bar_y, SCR_W, 1, col_white, 30);

    /* Start: a square the full height of the bar, flush to the corner. */
    int start_w = T.start_w;

    if (start_menu_open)
        fb_fill_rect(0, bar_y, start_w, TASKBAR_H, col_btn_on);

    /* Four panes, which is the Windows mark reduced to its essentials. */
    int gx = 14;
    int gy = bar_y + TASKBAR_H / 2 - 9;
    int gs = 8;

    fb_fill_rect(gx,          gy,          gs, gs, col_white);
    fb_fill_rect(gx + gs + 2, gy,          gs, gs, col_white);
    fb_fill_rect(gx,          gy + gs + 2, gs, gs, col_white);
    fb_fill_rect(gx + gs + 2, gy + gs + 2, gs, gs, col_white);

    /* Search field. It is decoration: there is nothing to search yet, and it
     * says so rather than accepting input that would go nowhere. */
    int search_x = T.search_x;
    int search_w = T.search_w;

    fb_fill_rect(search_x, bar_y + 5, search_w, TASKBAR_H - 10, col_search);

    /* A magnifier: a circle outline with a stroke off one corner. */
    int mx = search_x + 16, my = bar_y + TASKBAR_H / 2;
    fb_blend_round_rect(mx - 6, my - 6, 12, 12, 6, col_search_fg, 200);
    fb_fill_rect(mx - 3, my - 3, 6, 6, col_search);
    for (int i = 0; i < 5; i++)
        fb_fill_rect(mx + 4 + i, my + 4 + i, 2, 2, col_search_fg);

    fb_text_aa(search_x + 34, text_y, "Type here to search", col_search_fg, true);

    /* File Explorer, pinned. It sits next to the search field where Windows
     * puts it, and it is this window's only taskbar button rather than an
     * extra one beside a labelled one. */
    int fi = window_of_kind(WIN_FILES);
    bool files_open = fi >= 0 && windows[fi].visible;

    if (files_open)
        fb_blend_rect(T.pin_x, bar_y + 1, T.pin_w, TASKBAR_H - 1, col_white,
                      fi == focused ? 26 : 14);

    draw_folder(T.pin_x + 14, bar_y + TASKBAR_H / 2 - 9, 24, 19,
                col_folder, col_folder_tab);
    fb_text_aa(T.pin_x + 48, text_y, "File Explorer", col_white, true);

    if (files_open)
        fb_fill_rect(T.pin_x + 2, bar_y + TASKBAR_H - 3, T.pin_w - 4, 3,
                     fi == focused ? col_accent : col_title_off);

    /* Then one button per remaining window, flat, with the same underline. */
    int bx    = T.btn_x;
    int btn_w = T.btn_w;
    int clock_w = T.clock_w;

    for (int i = 0; i < window_count; i++) {
        if (windows[i].kind == WIN_FILES)
            continue;
        if (bx + btn_w > SCR_W - clock_w)
            break;

        bool open = windows[i].visible;
        bool front = open && (i == focused);

        if (open)
            fb_blend_rect(bx, bar_y + 1, btn_w, TASKBAR_H - 1,
                          col_white, front ? 26 : 14);

        fb_text_aa(bx + 14, text_y, windows[i].title, col_white, true);

        if (open)
            fb_fill_rect(bx + 2, bar_y + TASKBAR_H - 3, btn_w - 4, 3,
                         front ? col_accent : col_title_off);

        bx += btn_w + 2;
    }

    /* Clock and date, right-aligned. Both lines share a right edge, so each
     * is measured and offset rather than being placed at a fixed column --
     * the interface face is proportional. */
    rtc_time_t now;
    rtc_read(&now);

    char clock[8];
    clock[0] = (char)('0' + now.hour / 10);
    clock[1] = (char)('0' + now.hour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + now.minute / 10);
    clock[4] = (char)('0' + now.minute % 10);
    clock[5] = '\0';

    char date[16];
    int n = 0;
    date[n++] = (char)('0' + now.day / 10);
    date[n++] = (char)('0' + now.day % 10);
    date[n++] = '/';
    date[n++] = (char)('0' + now.month / 10);
    date[n++] = (char)('0' + now.month % 10);
    date[n++] = '/';
    date[n++] = (char)('0' + (now.year / 1000) % 10);
    date[n++] = (char)('0' + (now.year / 100) % 10);
    date[n++] = (char)('0' + (now.year / 10) % 10);
    date[n++] = (char)('0' + now.year % 10);
    date[n]   = '\0';

    int cw = fb_text_width(clock, true);
    int dw = fb_text_width(date, true);
    int right = SCR_W - 16;

    fb_text_aa(right - cw, bar_y + TASKBAR_H / 2 - lh, clock, col_white, true);
    fb_text_aa(right - dw, bar_y + TASKBAR_H / 2 + 1, date, col_white, true);
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
#define CURSOR_H 16

static uint32_t cursor_backing[CURSOR_W * 4 * CURSOR_H * 4];
static int      cursor_saved_x, cursor_saved_y;
static int      cursor_saved_w, cursor_saved_h;
static bool     cursor_visible;

/* The standard Windows arrow.
 *
 * The previous pointer was drawn from a handful of fill calls, which made it
 * a shape nobody recognises. This is the classic arrow: a white body with a
 * one-pixel black outline and a tail off the lower right.
 *
 * The outline is the part that matters and is why every desktop draws its
 * pointer this way. A solid white arrow disappears against a pale window and
 * a solid black one disappears against this wallpaper; an outlined one is
 * legible on both, without the compositor having to know what is underneath.
 *
 * The hotspot -- the pixel the click actually lands on -- is the tip at
 * (0, 0), which is also where Windows puts it.
 *
 *   X = outline, . = body, space = transparent
 */
#define CURSOR_ROWS 16
#define CURSOR_COLS 16

static const char *const cursor_pixels[CURSOR_ROWS] = {
    "X               ",
    "XX              ",
    "X.X             ",
    "X..X            ",
    "X...X           ",
    "X....X          ",
    "X.....X         ",
    "X......X        ",
    "X.......X       ",
    "X........X      ",
    "X.....XXXXX     ",
    "X..X..X         ",
    "X.X X..X        ",
    "XX  X..X        ",
    "X    X..X       ",
    "      XX        ",
};

static void cursor_shape(int mx, int my, int s)
{
    for (int row = 0; row < CURSOR_ROWS; row++) {
        const char *line = cursor_pixels[row];

        for (int col = 0; col < CURSOR_COLS && line[col]; col++) {
            char c = line[col];

            if (c == ' ')
                continue;       /* transparent: leave what is underneath */

            fb_fill_rect(mx + col * s, my + row * s, s, s,
                         c == 'X' ? col_black : col_white);
        }
    }
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
static void paint_window(int i);

/* Do two rectangles share any pixel? Used to decide which windows a damaged
 * region actually exposes, so the rest are left alone. */
static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw &&
           ay < by + bh && by < ay + ah;
}

/* Repaint everything that shows through a rectangle: the wallpaper inside it,
 * then any window that overlaps it, in z-order, then the taskbar if it reaches
 * that far. Windows are repainted whole rather than clipped -- a window that
 * pokes out of the region is drawn identically to what is already on screen,
 * so the extra pixels are wasted work but never wrong. */
static void repaint_region(int x, int y, int w, int h, int except)
{
    int desk_h = SCR_H - TASKBAR_H;

    wallpaper_draw_clip(SCR_W, desk_h, x, y, w, h);

    for (int i = 0; i < window_count; i++) {
        if (i == except || !windows[i].visible)
            continue;
        if (!rects_overlap(x, y, w, h,
                           windows[i].x, windows[i].y,
                           windows[i].w, windows[i].h))
            continue;

        paint_window(i);
    }

    if (y + h > desk_h)
        draw_taskbar();
}

/* Repaint the part of `old` that `new` does not cover.
 *
 * Restoring the whole of the old rectangle redraws wallpaper that the window
 * is about to cover again anyway. On a slow drag the genuinely uncovered part
 * is a strip a few pixels wide, and the difference is most of the work.
 */
static void repaint_strip(int x, int y, int w, int h, int except, bool push)
{
    if (w <= 0 || h <= 0)
        return;

    repaint_region(x, y, w, h, except);

    /* Pushed individually when the caller has nothing else to present. The
     * bounding box of the strips is the whole old rectangle, so presenting
     * them together would hand back everything the strips save. */
    if (push)
        fb_copy_rect(x, y, w, h);
}

static void repaint_uncovered(int ox, int oy, int w, int h,
                              int nx, int ny, int except, bool push)
{
    /* No overlap at all: the whole of the old position is exposed. */
    if (!rects_overlap(ox, oy, w, h, nx, ny, w, h)) {
        repaint_strip(ox, oy, w, h, except, push);
        return;
    }

    /* The band above or below the new position. */
    if (ny > oy)
        repaint_strip(ox, oy, w, ny - oy, except, push);
    else if (ny < oy)
        repaint_strip(ox, ny + h, w, oy - ny, except, push);

    /* The band to the left or right, limited to the rows the two share so the
     * corner is not painted twice. */
    int y0 = oy > ny ? oy : ny;
    int y1 = (oy + h) < (ny + h) ? (oy + h) : (ny + h);

    if (y1 > y0) {
        if (nx > ox)
            repaint_strip(ox, y0, nx - ox, y1 - y0, except, push);
        else if (nx < ox)
            repaint_strip(nx + w, y0, ox - nx, y1 - y0, except, push);
    }
}

static void drag_release(void)
{
    if (drag_cache) {
        kfree(drag_cache);
        drag_cache = 0;
    }

    drag_cache_w = drag_cache_h = 0;
    drag_capture = false;
    drag_moved   = false;
}

/* One drag step. Returns false if the pixels were not available, in which case
 * the caller falls back to rebuilding the scene. */
static bool draw_drag_step(int old_cx, int old_cy, int old_cw, int old_ch,
                           bool had_cursor)
{
    window_t *win = &windows[dragging];

    if (drag_capture) {
        drag_release();

        drag_cache = (uint32_t *)kmalloc((uint32_t)win->w * (uint32_t)win->h * 4);

        if (!drag_cache)
            return false;       /* no memory: the slow path still works */

        drag_cache_w = win->w;      /* dragging never resizes, so these hold */
        drag_cache_h = win->h;

        /* Read from where the window was last *painted*, not from where it
         * is now.
         *
         * The pointer has already moved by the time this runs -- input is
         * handled earlier in the same iteration -- so win->x is the
         * destination, and the backbuffer there still holds whatever was
         * behind the window. Capturing from it grabbed wallpaper and a strip
         * of the next window along, then blitted that as though it were the
         * window being dragged.
         *
         * The cursor is already erased at this point, so the pointer is not
         * baked into the cached pixels and smeared across the drag. */
        fb_read_rect(drag_prev_x, drag_prev_y,
                     drag_cache_w, drag_cache_h, drag_cache);

        drag_capture = false;
    }

    if (!drag_cache)
        return false;

    /* The pointer moves at the rate the mouse reports; the window does not
     * have to.
     *
     * Every window frame rewrites the whole of it into video memory -- for a
     * near-fullscreen window that is megabytes of uncached, hypervisor-
     * intercepted writes -- and the mouse reports faster than that is worth
     * doing. Real hardware sidesteps this with a cursor plane the display
     * controller overlays for free, so the pointer never costs a redraw at
     * all. This is the same split done in software: the pointer is repainted
     * every packet, the window at 50 Hz, and the two rectangles are tiny and
     * huge respectively.
     */
    if (drag_moved && (int32_t)(pit_ticks() - drag_last_frame) < 2) {
        cursor_draw();

        if (had_cursor)
            fb_copy_rect(old_cx, old_cy, old_cw, old_ch);

        fb_copy_rect(cursor_saved_x, cursor_saved_y,
                     cursor_saved_w, cursor_saved_h);
        fb_reset_dirty();
        return true;        /* the window catches up on the next frame */
    }

    int ox = drag_prev_x, oy = drag_prev_y;

    bool moved = (win->x != ox || win->y != oy);

    /* Let the adapter move the pixels if it can.
     *
     * SVGA_CMD_RECT_COPY moves a rectangle inside video memory without the
     * CPU reading or writing any of it. That is the whole cost of dragging a
     * large window: the backbuffer write is ordinary cached RAM and cheap,
     * but the copy into video memory is uncached and, under a hypervisor,
     * intercepted per access.
     *
     * The backbuffer is still updated, because every later partial repaint
     * reads from it and it has to agree with what is on screen.
     */


    bool accel = accel_moves && moved && svga_available() && svga_can_copy();

    if (moved) {
        if (accel) {
            /* The pointer is erased in the backbuffer but not yet on screen.
             * Push that erase first, or the adapter copies the old pointer
             * along with the window and smears it across the desktop. */
            if (had_cursor)
                fb_copy_rect(old_cx, old_cy, old_cw, old_ch);

            svga_copy_rect(ox, oy, win->x, win->y,
                           drag_cache_w, drag_cache_h);

            /* The FIFO runs asynchronously. The strips written next are CPU
             * writes straight into video memory, and some of them touch the
             * rectangle the adapter is still reading, so the two have to be
             * ordered. */
            svga_sync();
        }

        /* Uncover first, then put the window back down on top of it.
         *
         * repaint_region paints whole windows, not just the part inside the
         * damaged strip, so a neighbour that overlaps the dragged window gets
         * drawn across it. Blitting the window first meant every frame of a
         * drag ended with a neighbour painted over it, and the window only
         * returned to the front on release, when the full rebuild put the
         * z-order back. The order here is the z-order: background, then the
         * windows under it, then the one being dragged. */
        repaint_uncovered(ox, oy, drag_cache_w, drag_cache_h,
                          win->x, win->y, dragging, accel);

        fb_write_rect(win->x, win->y, drag_cache_w, drag_cache_h, drag_cache);

        /* The taskbar is above every window, which the full rebuild gets
         * right by drawing it last. Dragging a window across it has to do
         * the same, or the window covers the bar while it moves and the bar
         * reappears on release -- the same flip that painting the window
         * under its neighbours caused. */
        if (win->y + drag_cache_h > SCR_H - TASKBAR_H) {
            draw_taskbar();

            if (start_menu_open)
                draw_start_menu();

            if (accel)
                fb_copy_rect(0, SCR_H - TASKBAR_H, SCR_W, TASKBAR_H);
        }
    }

    cursor_draw();

    if (accel) {
        /* The window never went through the CPU: only the pointer and the
         * uncovered strips did, and those have been pushed already. */
        fb_copy_rect(cursor_saved_x, cursor_saved_y,
                     cursor_saved_w, cursor_saved_h);
        fb_reset_dirty();

        drag_prev_x = win->x;
        drag_prev_y = win->y;
        drag_moved  = false;
        drag_last_frame = pit_ticks();

        return true;
    }

    /* Push the rectangles that changed, not the box that contains them.
     *
     * fb_present copies the bounding box of everything marked dirty, and here
     * that box spans the old window position, the new one, and both pointer
     * positions -- most of the desktop. Video memory is uncached and, under a
     * hypervisor, every write is intercepted, so the bytes copied are the
     * cost. Worse, a copy that large is not finished within one scanout, so
     * the display shows it half-applied and the window appears to vanish and
     * refill.
     *
     * Four disjoint copies instead. The old pointer rectangle is one of them:
     * leaving it out is what smeared ghost cursors across the screen the
     * first time this was written, because cursor_erase cleans the backbuffer
     * where the pointer used to be and nothing then copied it forward. */
    if (had_cursor)
        fb_copy_rect(old_cx, old_cy, old_cw, old_ch);

    fb_copy_rect(ox, oy, drag_cache_w, drag_cache_h);
    fb_copy_rect(win->x, win->y, drag_cache_w, drag_cache_h);
    fb_copy_rect(cursor_saved_x, cursor_saved_y,
                 cursor_saved_w, cursor_saved_h);
    fb_reset_dirty();

    drag_prev_x = win->x;
    drag_prev_y = win->y;
    drag_moved  = false;
    drag_last_frame = pit_ticks();

    return true;
}

static void gui_flush_terminal(void)
{
    term_last_flush = pit_ticks();

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

/* Kernel Lab's equivalent, for the same reason: an experiment blocks
 * the render loop while it runs, and this is what keeps its output visibly
 * arriving in the meantime. */
static void gui_flush_demos(void)
{
    dlog_last_flush = pit_ticks();

    for (int i = 0; i < window_count; i++) {
        if (windows[i].kind != WIN_DEMOS || !windows[i].visible)
            continue;

        cursor_erase();
        draw_demos(&windows[i], i == focused);
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
        int sx, sy, mw, mh, rh;
        start_menu_box(&sx, &sy, &mw, &mh, &rh);

        if (in_rect(mx, my, sx, sy, mw, mh)) {
            int item = (my - sy - 8) / rh;
            start_menu_open = false;

            if (item == 0) show_window(WIN_TERMINAL);
            else if (item == 1) show_window(WIN_FILES);
            else if (item == 2) show_window(WIN_TASKS);
            else if (item == 3) show_window(WIN_NOTEPAD);
            else if (item == 4) show_window(WIN_DEMOS);
            else if (item == 5) show_window(WIN_STATS);
            else if (item == 6) wallpaper_next();
            else if (item == 7) return false;

            return true;
        }

        start_menu_open = false;   /* clicked away: dismiss */
    }

    if (my >= bar_y) {
        taskbar_layout_t T;
        taskbar_layout(&T);

        if (mx < T.start_w) {
            start_menu_open = !start_menu_open;
            return true;
        }

        int hit = -1;

        if (in_rect(mx, my, T.pin_x, bar_y, T.pin_w, TASKBAR_H))
            hit = window_of_kind(WIN_FILES);
        else
            hit = taskbar_button_window(&T, mx, my);

        if (hit >= 0) {
            windows[hit].visible = !windows[hit].visible;
            if (windows[hit].visible) {
                raise_window(hit);
                focused = window_count - 1;
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

        if (windows[i].kind == WIN_FILES && files_click(&windows[i], mx, my)) {
            if (files_open_notepad) {
                files_open_notepad = false;
                show_window(WIN_NOTEPAD);
                return true;
            }
            raise_window(i);
            focused = window_count - 1;
            return true;
        }

        if (windows[i].kind == WIN_DEMOS && demos_click(&windows[i], mx, my)) {
            raise_window(i);
            focused = window_count - 1;
            return true;
        }

        if (windows[i].kind == WIN_TASKS && tm_click(&windows[i], mx, my)) {
            raise_window(i);
            focused = window_count - 1;
            return true;
        }

        if (windows[i].kind == WIN_NOTEPAD && np_click(&windows[i], mx, my)) {
            raise_window(i);
            focused = window_count - 1;
            return true;
        }

        bool on_title = in_rect(mx, my, windows[i].x, windows[i].y,
                                windows[i].w, TITLE_H + BORDER);

        raise_window(i);
        focused = window_count - 1;

        if (on_title) {
            dragging = focused;
            drag_capture = true;
            drag_moved   = false;
            drag_prev_x  = windows[dragging].x;
            drag_prev_y  = windows[dragging].y;
            drag_dx  = mx - windows[dragging].x;
            drag_dy  = my - windows[dragging].y;
        }
        return true;
    }

    return true;
}

static void paint_window(int i)
{
    if (i < 0 || i >= window_count || !windows[i].visible)
        return;

    draw_window_frame(&windows[i], i == focused);

    if (windows[i].kind == WIN_TERMINAL)
        draw_terminal(&windows[i], i == focused);
    else if (windows[i].kind == WIN_FILES)
        draw_files(&windows[i], i == focused);
    else if (windows[i].kind == WIN_DEMOS)
        draw_demos(&windows[i], i == focused);
    else if (windows[i].kind == WIN_TASKS)
        draw_tm(&windows[i], i == focused);
    else if (windows[i].kind == WIN_NOTEPAD)
        draw_np(&windows[i], i == focused);
    else
        draw_stats(&windows[i]);
}

static bool window_is_live_panel(int i)
{
    window_kind_t k;

    if (i < 0 || i >= window_count || !windows[i].visible)
        return false;

    k = windows[i].kind;
    if (k == WIN_STATS || k == WIN_TASKS)
        return true;
    if (k == WIN_DEMOS)
        return true;    /* hidden unless opened; cheap when not visible */
    return false;
}

/* Shortest period among the panels that are actually live. The taskbar
 * clock is always on, so this is never "sleep forever": the CPU still
 * halts between deadlines. A 3 s Task Manager period looked frozen at
 * rest, and a click only painted one frame because last_frame was then
 * reset. 1 Hz is enough to see ticks, uptime and the CPU ring move;
 * faster than that and the blit shows up as kernel time in the same
 * window. */
static uint32_t gui_refresh_ticks(void)
{
    uint32_t refresh = 100;     /* taskbar / System: 1 s at 100 Hz */

    if (pc_live_running())
        refresh = 5;

    if (task_others_ready() && refresh > 25)
        refresh = 25;

    {
        deadlock_info_t dl;
        deadlock_snapshot(&dl);
        if (dl.active && !dl.finished && refresh > 5)
            refresh = 5;
    }

    {
        desktop_hog_info_t hog;
        desktop_hog_snapshot(&hog);
        if (hog.running && hog.sharing && refresh > 8)
            refresh = 8;
    }

    int di = window_of_kind(WIN_DEMOS);
    if (di >= 0 && windows[di].visible
        && (demo_entries[demo_sel].kind == ENTRY_SCHED
         || demo_entries[demo_sel].kind == ENTRY_MMU)
        && refresh > 8)
        refresh = 8;

    return refresh;
}

static bool gui_input_pending(void)
{
    return kbd_available() || mouse_has_pending_input();
}

/* Park until input or the panel-refresh deadline. Workers that are READY
 * still run: pick_next prefers them over idle, and this caller is BLOCKED
 * rather than sitting in the run queue. Yielding here used to keep the GUI
 * in round-robin with the workers, so the kernel row read ~40% under load
 * for doing nothing. Input IRQs and the deadline still wake us. */
static void gui_wait(uint32_t deadline)
{
    if (gui_input_pending())
        return;

    uint32_t now = pit_ticks();

    /* A frame that overran its period would otherwise return immediately,
     * and with `dirty` stuck true the loop would spin at 100%. Halt for
     * at least one tick so the idle task can take the CPU. */
    if ((int32_t)(deadline - now) <= 0)
        deadline = now + 1;

    task_idle_wait(deadline);
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
    col_search    = fb_rgb(60,  64,  76);
    col_search_fg = fb_rgb(186, 190, 200);
    col_menu      = fb_rgb(32,  36,  46);
    col_menu_fg   = fb_rgb(238, 240, 245);
    col_head      = fb_rgb(232, 234, 239);
    col_list_bg   = fb_rgb(252, 252, 253);
    col_list_sel  = fb_rgb(58,  110, 178);
    col_list_sel_off = fb_rgb(214, 220, 230);
    col_pane      = fb_rgb(24,  28,  36);
    col_paper     = fb_rgb(250, 250, 252);
    col_folder    = fb_rgb(232, 184, 92);
    col_folder_tab= fb_rgb(206, 158, 66);
    col_ok        = fb_rgb(88,  150, 105);
    col_white     = fb_rgb(255, 255, 255);
    col_black     = fb_rgb(0,   0,   0);

    /* The stats window is sized first, from the longest label it has to show,
     * and the terminal then takes the width that is left. Sizing the terminal
     * first as a fraction of the screen left the two overlapping on startup:
     * correct behaviour from the window manager, but a poor first impression. */
    /* A column down the right-hand side holds the stats panel and the file
     * manager; the terminal takes the rest. Three windows that open on top of
     * each other are correct behaviour from a window manager and a poor first
     * impression, so the initial arrangement does not overlap. */
    int rw = (SCR_W * 43) / 100;

    if (rw < 26 * CHROME_W) rw = 26 * CHROME_W;
    if (rw > SCR_W / 2)     rw = SCR_W / 2;

    int sw = rw;
    int sh = 5 * (CHROME_H + 5) + TITLE_H + 24;

    int tw = SCR_W - sw - 34;
    int th = SCR_H - TASKBAR_H - 60;

    if (tw < 40 * CELL_W) {
        tw = SCR_W - 20;        /* too narrow to sit alongside; use full width */
        rw = SCR_W - 24;
        sw = rw;
    }

    term_cols = (tw - 2 * BORDER - 8) / CELL_W;
    term_rows = (th - TITLE_H - 2 * BORDER - 8) / CELL_H;

    if (term_cols > MAX_COLS) term_cols = MAX_COLS;
    if (term_rows > MAX_ROWS) term_rows = MAX_ROWS;
    if (term_cols < 20) term_cols = 20;
    if (term_rows < 6)  term_rows = 6;

    tw = term_cols * CELL_W + 2 * BORDER + 8;
    th = term_rows * CELL_H + TITLE_H + 2 * BORDER + 8;

    term_clear();

    int fy = 30 + sh + 14;
    int fh = SCR_H - TASKBAR_H - fy - 20;

    if (fh < 6 * (CHROME_H + 8))
        fh = 6 * (CHROME_H + 8);

    /* Kernel Lab starts closed, like Task Manager and Notepad: open it from
     * the Start menu or the taskbar. */
    int dw = (SCR_W * 62) / 100;
    int dh = ((SCR_H - TASKBAR_H) * 82) / 100;
    int tmw = (SCR_W * 52) / 100;
    int tmh = ((SCR_H - TASKBAR_H) * 58) / 100;
    int npw = (SCR_W * 48) / 100;
    int nph = ((SCR_H - TASKBAR_H) * 62) / 100;

    window_count = 6;
    windows[0] = (window_t){ (SCR_W - dw) / 2, 36, dw, dh,
                             "Kernel Lab", WIN_DEMOS, false };
    windows[1] = (window_t){ SCR_W - sw - 12, 30, sw, sh,
                             "System", WIN_STATS, true };
    windows[2] = (window_t){ SCR_W - rw - 12, fy, rw, fh,
                             "File Explorer", WIN_FILES, true };
    windows[3] = (window_t){ 10, 40, tw, th,
                             "Terminal", WIN_TERMINAL, true };
    windows[4] = (window_t){ 48, 56, tmw, tmh,
                             "Task Manager", WIN_TASKS, false };
    windows[5] = (window_t){ 90, 80, npw, nph,
                             "Notepad", WIN_NOTEPAD, false };
    focused = 3;

    files_sel = 0;
    files_scroll = 0;
    tm_sel = 0;
    tm_scroll = 0;
    tm_msg[0] = '\0';
    np_len = 0;
    np_cursor = 0;
    np_scroll = 0;
    np_name[0] = '\0';
    np_saved_name[0] = '\0';
    np_edit_name[0] = '\0';
    np_edit_cursor = 0;
    np_naming = false;
    np_dirty = false;
    np_picker = false;
    np_msg[0] = '\0';

    demo_sel = 1;       /* entry 0 is a section header */
    demo_scroll = 0;

    for (int i = 0; i < window_count; i++)
        clamp_window(&windows[i]);

    /* Size Kernel Lab's output grid once. The window cannot be resized,
     * and the wrap column has to exist before the first character arrives.
     * The depth is what remains of the pane under the tallest description
     * and its button, less room for the result graphics above the log. */
    {
        demos_layout_t L;

        dlog_rows = 0;
        demos_layout(&windows[0], &L);

        int deepest = L.desc_y + 4 * (CHROME_H + 4) + 8 + L.btn_h + 12;
        int space   = L.pane_bottom - deepest - (5 * (CHROME_H + 6) + 24);
        int rows    = (space - 12) / CELL_H;

        if (rows < 4)             rows = 4;
        if (rows > DLOG_MAX_ROWS) rows = DLOG_MAX_ROWS;
        dlog_rows = rows;

        dlog_cols = (L.pane_w - 16) / CELL_W;
        if (dlog_cols < 20)            dlog_cols = 20;
        if (dlog_cols > DLOG_MAX_COLS) dlog_cols = DLOG_MAX_COLS;

        dlog_clear();
    }

    start_menu_open = false;
    dragging = -1;

    mouse_set_bounds(SCR_W, SCR_H);
    gui_active = true;
    console_set_sink(term_putc);

    kprintf("TazOS graphical mode - %dx%d\n", SCR_W, SCR_H);
    kprintf("Start menu opens windows. Drag by the title bar.\n");
    kprintf("try: help, tasks, meminfo\n\n> ");

    /* Two different notions of "needs work". scene_dirty means the windows or
     * their contents changed and the interface has to be repainted. dirty
     * means only that something happened at all -- most often the pointer
     * moving, which needs the cursor recomposited and nothing else. */
    bool     scene_dirty = true;
    bool     panels_dirty = false;
    bool     dirty = true;
    int      last_mx = mouse_x(), last_my = mouse_y();
    uint32_t last_frame = pit_ticks();

    for (;;) {
        desktop_hog_poll();

        while (kbd_available()) {
            char c = kbd_poll();

            if (c == 27) {
                if (focused >= 0 && focused < window_count
                    && windows[focused].visible
                    && windows[focused].kind == WIN_NOTEPAD
                    && (np_picker || np_naming)) {
                    np_key(c);
                } else {
                    gui_active = false;
                    console_set_sink(0);
                    fbcon_clear();
                    return;
                }
            } else if (focused >= 0 && focused < window_count
                && windows[focused].visible
                && windows[focused].kind == WIN_NOTEPAD) {
                np_key(c);
            } else if (c == '\n') {
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

                /* Deliberately not scene_dirty: the scene has not changed,
                 * one rectangle in it has moved. */
                dirty = true;
                drag_moved = true;
            } else {
                dragging = -1;
                drag_release();
                scene_dirty = true;
                dirty = true;
            }
        }

        /* The wheel scrolls whichever terminal the pointer is over, rather
         * than the focused window. That is what every desktop does, and it
         * means you do not have to click a window before it will respond. */
        int wheel = mouse_take_wheel();
        if (wheel != 0) {
            for (int i = window_count - 1; i >= 0; i--) {
                if (!windows[i].visible)
                    continue;
                if (!in_rect(mouse_x(), mouse_y(), windows[i].x, windows[i].y,
                             windows[i].w, windows[i].h))
                    continue;

                if (windows[i].kind == WIN_TERMINAL) {
                    term_scroll_view(wheel * 3);
                } else if (windows[i].kind == WIN_FILES) {
                    files_scroll -= wheel * 2;
                    if (files_scroll < 0)
                        files_scroll = 0;
                } else if (windows[i].kind == WIN_DEMOS) {
                    demo_scroll -= wheel * 2;   /* upper bound applied in draw */
                    if (demo_scroll < 0)
                        demo_scroll = 0;
                } else if (windows[i].kind == WIN_TASKS) {
                    tm_scroll -= wheel * 2;
                    if (tm_scroll < 0)
                        tm_scroll = 0;
                } else if (windows[i].kind == WIN_NOTEPAD) {
                    if (np_picker) {
                        np_pick_scroll -= wheel * 2;
                        if (np_pick_scroll < 0)
                            np_pick_scroll = 0;
                    } else {
                        np_scroll -= wheel * 2;
                        if (np_scroll < 0)
                            np_scroll = 0;
                    }
                } else {
                    break;      /* nothing in this window scrolls */
                }

                dirty = true;
                scene_dirty = true;
                break;
            }
        }

        if (mouse_x() != last_mx || mouse_y() != last_my) {
            last_mx = mouse_x();
            last_my = mouse_y();
            dirty = true;
        }

        uint32_t refresh = gui_refresh_ticks();

        if (pit_ticks() - last_frame >= refresh) {
            dirty = true;
            /* Clock, Task Manager and the stats panel read live kernel
             * state. Repainting only those windows leaves the wallpaper
             * and the rest of the scene in the backbuffer. A full rebuild
             * here was the 1920x1080 cost of moving a clock. */
            panels_dirty = true;
        }

        if (!dirty) {
            gui_wait(last_frame + refresh);
            continue;
        }

        /* The cursor sits on top of the scene, so it has to come off before
         * anything underneath is touched, and go back on afterwards. */
        int cx = cursor_saved_x, cy = cursor_saved_y;
        int cw = cursor_saved_w, ch = cursor_saved_h;
        bool had_cursor = cursor_visible;
        bool painted_panels = scene_dirty || panels_dirty;

        cursor_erase();

        if (dragging >= 0 && !scene_dirty &&
            draw_drag_step(cx, cy, cw, ch, had_cursor)) {
            /* Nothing further: the step pushed its own rectangles. */
        } else if (scene_dirty) {
            scene_dirty = false;
            panels_dirty = false;

            /* The wallpaper draws itself; nothing is written over it.
             * A desktop is a backdrop, and a caption sitting on it is the
             * sort of thing only a demo has. */
            wallpaper_draw(SCR_W, SCR_H - TASKBAR_H);

            for (int i = 0; i < window_count; i++)
                paint_window(i);

            draw_taskbar();

            if (start_menu_open)
                draw_start_menu();

            cursor_draw();
            fb_present();
        } else if (panels_dirty) {
            panels_dirty = false;

            for (int i = 0; i < window_count; i++)
                if (window_is_live_panel(i))
                    paint_window(i);

            draw_taskbar();

            if (start_menu_open)
                draw_start_menu();

            cursor_draw();

            /* Copy each live window and the taskbar on their own. One dirty
             * box spanning Task Manager on the left and System on the right
             * is most of the desktop, which is how a clock update used to
             * cost a 1920x1080 blit. */
            if (had_cursor)
                fb_copy_rect(cx, cy, cw, ch);
            for (int i = 0; i < window_count; i++)
                if (window_is_live_panel(i))
                    fb_copy_rect(windows[i].x, windows[i].y,
                                 windows[i].w, windows[i].h);
            fb_copy_rect(0, SCR_H - TASKBAR_H, SCR_W, TASKBAR_H);
            if (start_menu_open) {
                int mx, my, mw, mh, rh;
                start_menu_box(&mx, &my, &mw, &mh, &rh);
                fb_copy_rect(mx, my, mw, mh);
            }
            fb_copy_rect(cursor_saved_x, cursor_saved_y,
                         cursor_saved_w, cursor_saved_h);
            fb_reset_dirty();
        } else {
            /* Moving the pointer changes two small rectangles: where it was
             * and where it is. Their bounding box, which is what fb_present
             * would copy, grows with how fast the mouse is moving -- a quick
             * flick across the screen turned a 4 KB update into a full-width
             * one, and that copy is slow enough to be seen as the pointer
             * blinking out. */
            cursor_draw();

            if (had_cursor)
                fb_copy_rect(cx, cy, cw, ch);

            fb_copy_rect(cursor_saved_x, cursor_saved_y,
                         cursor_saved_w, cursor_saved_h);
            fb_reset_dirty();
        }

        /* One present per frame for the full-scene and cursor-only paths.
         * Presenting between the erase and the redraw seemed tidier -- it
         * keeps the dirty box small when the pointer jumps a long way -- but
         * it puts a frame on screen with the cursor missing, and at this
         * frame rate that reads as a flicker. Doing it in one pass costs a
         * larger rectangle occasionally and never shows a half-drawn state.
         *
         * `dirty` has to be consumed here. Leaving it set made every pass a
         * present even when nothing had changed, which is the 100% CPU the
         * idle task exists to end.
         *
         * last_frame counts panel/scene paints only. A cursor-only frame
         * used to stamp it, so a twitchy mouse (or the click that woke
         * us) postponed the periodic refresh indefinitely: Task Manager
         * painted once, then froze until the next click. */
        bool painted = painted_panels;
        dirty = false;
        if (painted)
            last_frame = pit_ticks();
        gui_wait(last_frame + refresh);
    }
}
