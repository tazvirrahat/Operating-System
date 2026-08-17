#include "gui.h"
#include "gfx.h"
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

#define TITLE_H     10
#define BORDER      1
#define MAX_WINDOWS 3

/* What a window contains. Stored explicitly rather than inferred from the
 * array index, because raising a window to the front reorders the array —
 * identifying windows by position would mean the contents swap places the
 * first time one is clicked. */
typedef enum {
    WIN_TERMINAL,
    WIN_STATS,
} window_kind_t;

typedef struct {
    int           x, y, w, h;
    const char   *title;
    window_kind_t kind;
    bool          visible;
} window_t;

/* Drawn back to front, so later entries appear on top. Dragging moves the
 * dragged window to the end of this list. */
static window_t windows[MAX_WINDOWS];
static int      window_count;
static int      focused = -1;

static int  dragging = -1;      /* index of the window being dragged */
static int  drag_dx, drag_dy;   /* grab point within the title bar */

/* ---- terminal window ------------------------------------------------------
 *
 * A character grid that the console writes into. Shell output arrives here
 * because gui_run installs a console sink; the shell itself is unchanged and
 * unaware that its output is landing in a window.
 */

#define TERM_COLS 37
#define TERM_ROWS 11

static char term_cells[TERM_ROWS][TERM_COLS];
static int  term_cx, term_cy;

static void term_clear(void)
{
    memset(term_cells, ' ', sizeof(term_cells));
    term_cx = term_cy = 0;
}

static void term_scroll(void)
{
    for (int y = 1; y < TERM_ROWS; y++)
        memcpy(term_cells[y - 1], term_cells[y], TERM_COLS);

    memset(term_cells[TERM_ROWS - 1], ' ', TERM_COLS);
    term_cy = TERM_ROWS - 1;
}

static void term_putc(char c)
{
    if (c == '\n') {
        term_cx = 0;
        term_cy++;
    } else if (c == '\r') {
        term_cx = 0;
    } else if (c == '\b') {
        if (term_cx > 0) {
            term_cx--;
            term_cells[term_cy][term_cx] = ' ';
        }
    } else if (c == '\t') {
        term_cx = (term_cx + 4) & ~3;
    } else if (c >= 32) {
        term_cells[term_cy][term_cx++] = c;
    }

    if (term_cx >= TERM_COLS) {
        term_cx = 0;
        term_cy++;
    }

    if (term_cy >= TERM_ROWS)
        term_scroll();
}

/* ---- drawing -------------------------------------------------------------- */

static void draw_window(int index)
{
    window_t *win = &windows[index];
    if (!win->visible)
        return;

    bool active = (index == focused);

    /* Drop shadow, drawn first so the window sits over it. */
    gfx_fill_rect(win->x + 3, win->y + 3, win->w, win->h, C_WIN_SHADE);

    gfx_fill_rect(win->x, win->y, win->w, win->h, C_WIN_FACE);
    gfx_rect(win->x, win->y, win->w, win->h, C_BLACK);

    /* Title bar. */
    gfx_fill_rect(win->x + BORDER, win->y + BORDER,
                  win->w - 2 * BORDER, TITLE_H,
                  active ? C_TITLE_ON : C_TITLE_OFF);

    gfx_text(win->x + 4, win->y + 2, win->title, C_WHITE);

    /* A bevel along the top and left, which is most of what makes a flat
     * rectangle read as a raised surface. */
    gfx_hline(win->x + 1, win->y + TITLE_H + 1, win->w - 2, C_WIN_EDGE);
}

static void draw_terminal(const window_t *win, bool active)
{
    int tx = win->x + BORDER + 2;
    int ty = win->y + TITLE_H + 4;

    gfx_fill_rect(win->x + BORDER + 1, win->y + TITLE_H + 3,
                  win->w - 2 * BORDER - 2, win->h - TITLE_H - BORDER - 4,
                  C_TERM_BG);

    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            char c = term_cells[row][col];
            if (c != ' ')
                gfx_char(tx + col * GLYPH_W, ty + row * GLYPH_H, c, C_LGREEN);
        }
    }

    /* Block cursor, only while this window has focus. */
    if (active)
        gfx_fill_rect(tx + term_cx * GLYPH_W, ty + term_cy * GLYPH_H + 7,
                      GLYPH_W, 1, C_LGREEN);
}

static void draw_stats_window(const window_t *win)
{
    heap_stats_t heap;
    heap_get_stats(&heap);

    uint32_t secs = pit_hz() ? pit_ticks() / pit_hz() : 0;

    int tx = win->x + 5;
    int ty = win->y + TITLE_H + 5;

    char line[40];

    /* No snprintf here, so the numbers are formatted by hand. */
    const char *labels[4] = { "tasks:", "uptime:", "heap KB:", "switches:" };
    uint32_t    values[4] = { (uint32_t)task_count(), secs,
                              heap.used_bytes / 1024, task_switch_count() };

    for (int i = 0; i < 4; i++) {
        gfx_text(tx, ty + i * (GLYPH_H + 2), labels[i], C_BLACK);

        /* Render the value right of the label. */
        uint32_t v = values[i];
        int      p = 0;
        char     digits[12];

        if (v == 0)
            digits[p++] = '0';
        while (v > 0) {
            digits[p++] = (char)('0' + (v % 10));
            v /= 10;
        }

        int q = 0;
        while (p > 0)
            line[q++] = digits[--p];
        line[q] = '\0';

        gfx_text(tx + 72, ty + i * (GLYPH_H + 2), line, C_BLUE);
    }
}

static void draw_cursor(void)
{
    int mx = mouse_x();
    int my = mouse_y();

    /* A simple arrow: a black outline with a white interior, so it stays
     * visible over both the light window face and the dark desktop. */
    for (int i = 0; i < 10; i++) {
        gfx_pixel(mx, my + i, C_BLACK);
        if (i > 0 && i < 8)
            gfx_pixel(mx + 1, my + i, C_WHITE);
        if (i > 1 && i < 7)
            gfx_pixel(mx + 2, my + i, C_WHITE);
    }

    for (int i = 0; i < 6; i++)
        gfx_pixel(mx + 3 + i, my + 3 + i, i < 4 ? C_WHITE : C_BLACK);

    gfx_pixel(mx + 1, my + 8, C_BLACK);
    gfx_pixel(mx + 2, my + 7, C_BLACK);
}

static void draw_desktop(void)
{
    gfx_clear(C_DESKTOP);

    gfx_text(4, 4, "MyOS", C_WHITE);
    gfx_text(4, 14, "bare metal x86", C_LCYAN);

    gfx_text(4, GFX_HEIGHT - 10, "ESC exits to text mode", C_LGREY);
}

/* ---- input ---------------------------------------------------------------- */

static bool point_in_title(const window_t *win, int x, int y)
{
    return x >= win->x && x < win->x + win->w
        && y >= win->y && y < win->y + TITLE_H + BORDER;
}

static bool point_in_window(const window_t *win, int x, int y)
{
    return x >= win->x && x < win->x + win->w
        && y >= win->y && y < win->y + win->h;
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

static void handle_mouse(void)
{
    int mx = mouse_x();
    int my = mouse_y();

    if (mouse_take_click(MOUSE_LEFT)) {
        /* Front to back, so the topmost window under the pointer wins. */
        for (int i = window_count - 1; i >= 0; i--) {
            if (!windows[i].visible || !point_in_window(&windows[i], mx, my))
                continue;

            bool on_title = point_in_title(&windows[i], mx, my);

            /* Raising reorders the array, so the index this window is about
             * to live at is the last one. Capturing focus and the drag target
             * before raising would leave both pointing at whatever slid down
             * into the old position. */
            raise_window(i);
            focused = window_count - 1;

            if (on_title) {
                dragging = window_count - 1;
                drag_dx  = mx - windows[dragging].x;
                drag_dy  = my - windows[dragging].y;
            }
            break;
        }
    }

    if (dragging >= 0) {
        if (mouse_buttons() & MOUSE_LEFT) {
            windows[dragging].x = mx - drag_dx;
            windows[dragging].y = my - drag_dy;
        } else {
            dragging = -1;
        }
    }
}

/* ---- entry point ---------------------------------------------------------- */

void gui_run(void)
{
    char line[80];
    int  len = 0;

    term_clear();

    /* Later entries are drawn on top, so the terminal goes last and starts
     * focused. */
    /* Laid out so both are fully visible at startup. They can be dragged over
     * one another afterwards, which is what demonstrates the z-ordering. */
    window_count = 2;
    windows[0] = (window_t){ 174, 12, 138, 62,  "system",   WIN_STATS,    true };
    windows[1] = (window_t){ 6,   82, 308, 112, "terminal", WIN_TERMINAL, true };
    focused    = 1;

    gfx_enter();
    mouse_set_bounds(GFX_WIDTH, GFX_HEIGHT);

    /* From here, everything the shell prints lands in the terminal window. */
    console_set_sink(term_putc);

    kprintf("MyOS graphical mode\n");
    kprintf("try: help, tasks, uptime\n\n> ");

    for (;;) {
        while (kbd_available()) {
            char c = kbd_poll();

            if (c == 27) {          /* escape */
                console_set_sink(0);
                gfx_leave();
                return;
            }

            if (c == '\n') {
                kputc('\n');
                line[len] = '\0';
                shell_dispatch(line);
                len = 0;
                kprintf("> ");
            } else if (c == '\b') {
                if (len > 0) {
                    len--;
                    kputc('\b');
                }
            } else if (c >= 32 && c < 127 && len < (int)sizeof(line) - 1) {
                line[len++] = c;
                kputc(c);
            }
        }

        handle_mouse();

        draw_desktop();

        /* Back to front: index 0 is furthest back after any raising. */
        for (int i = 0; i < window_count; i++) {
            if (!windows[i].visible)
                continue;

            draw_window(i);

            if (windows[i].kind == WIN_TERMINAL)
                draw_terminal(&windows[i], i == focused);
            else
                draw_stats_window(&windows[i]);
        }

        draw_cursor();
        gfx_present();

        /* Yield rather than spin, so background tasks keep running and the
         * scheduler is visibly still doing its job while the GUI is up. */
        task_yield();
    }
}
