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
#define MAX_WINDOWS 3

/* Upper bounds on the terminal grid; the part actually used is computed from
 * the window size. */
#define MAX_COLS 160
#define MAX_ROWS 80

typedef enum { WIN_TERMINAL, WIN_STATS, WIN_FILES } window_kind_t;

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
static uint32_t col_search, col_search_fg;
static uint32_t col_menu, col_menu_fg;
static uint32_t col_head, col_list_bg, col_list_sel, col_list_sel_off;
static uint32_t col_pane, col_paper, col_folder, col_folder_tab;

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
#define START_ITEMS 5

static const char *start_items[START_ITEMS] = {
    "Terminal", "File Explorer", "System", "Change wallpaper", "Exit to console"
};

static void start_menu_box(int *x, int *y, int *w, int *h, int *row)
{
    int rh = CHROME_H + 16;

    *row = rh;
    *w   = 19 * CHROME_W;
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

    files_sel = row;
    return true;
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
 * has its own button, so it is not also given one here. */
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
            else if (item == 2) show_window(WIN_STATS);
            else if (item == 3) wallpaper_next();
            else if (item == 4) return false;

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

    window_count = 3;
    windows[0] = (window_t){ SCR_W - sw - 12, 30, sw, sh,
                             "System", WIN_STATS, true };
    windows[1] = (window_t){ SCR_W - rw - 12, fy, rw, fh,
                             "File Explorer", WIN_FILES, true };
    windows[2] = (window_t){ 10, 40, tw, th,
                             "Terminal", WIN_TERMINAL, true };
    focused = 2;

    files_sel = 0;
    files_scroll = 0;

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

        if (pit_ticks() - last_frame >= 25) {
            dirty = true;

            /* The stats panel and the file listing are both read from live
             * state rather than cached, so the periodic frame has to rebuild
             * the scene or neither would ever change. */
            scene_dirty = true;
        }

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

            /* The wallpaper draws itself; nothing is written over it.
             * A desktop is a backdrop, and a caption sitting on it is the
             * sort of thing only a demo has. */
            wallpaper_draw(SCR_W, SCR_H - TASKBAR_H);

            for (int i = 0; i < window_count; i++) {
                if (!windows[i].visible)
                    continue;

                draw_window_frame(&windows[i], i == focused);

                if (windows[i].kind == WIN_TERMINAL)
                    draw_terminal(&windows[i], i == focused);
                else if (windows[i].kind == WIN_FILES)
                    draw_files(&windows[i], i == focused);
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
