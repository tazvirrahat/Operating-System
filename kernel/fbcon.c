#include "fbcon.h"
#include "fb.h"
#include "heap.h"
#include "string.h"

#define CELL_W (8 * FBCON_SCALE)
#define CELL_H (8 * FBCON_SCALE)

static char *cells;             /* cols * rows character grid */
static int   cols, rows;
static int   cx, cy;
static bool  ready;

static uint32_t colour_fg;
static uint32_t colour_bg;

bool fbcon_init(void)
{
    if (!fb_available())
        return false;

    cols = (int)(fb_width()  / CELL_W);
    rows = (int)(fb_height() / CELL_H);

    cells = kmalloc((uint32_t)(cols * rows));
    if (!cells)
        return false;

    memset(cells, ' ', (uint32_t)(cols * rows));

    colour_bg = fb_rgb(12, 12, 20);
    colour_fg = fb_rgb(210, 210, 210);

    cx = cy = 0;
    ready = true;

    fb_clear(colour_bg);
    fb_present();

    return true;
}

bool fbcon_active(void) { return ready; }
int  fbcon_cols(void)   { return cols; }
int  fbcon_rows(void)   { return rows; }

static void draw_cell(int col, int row)
{
    int px = col * CELL_W;
    int py = row * CELL_H;

    fb_fill_rect(px, py, CELL_W, CELL_H, colour_bg);

    char c = cells[row * cols + col];
    if (c != ' ')
        fb_char(px, py, c, colour_fg, FBCON_SCALE);
}

/* Redrawing every cell is the expensive path, so it is used only when the
 * whole screen has changed. */
static void redraw_all(void)
{
    fb_clear(colour_bg);

    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++)
            if (cells[row * cols + col] != ' ')
                fb_char(col * CELL_W, row * CELL_H,
                        cells[row * cols + col], colour_fg, FBCON_SCALE);
}

static void scroll(void)
{
    memmove(cells, cells + cols, (uint32_t)((rows - 1) * cols));
    memset(cells + (rows - 1) * cols, ' ', (uint32_t)cols);

    cy = rows - 1;
    redraw_all();
}

void fbcon_putc(char c)
{
    if (!ready)
        return;

    switch (c) {
    case '\n':
        cx = 0;
        cy++;
        break;
    case '\r':
        cx = 0;
        break;
    case '\t':
        cx = (cx + 4) & ~3;
        break;
    case '\b':
        if (cx > 0) {
            cx--;
            cells[cy * cols + cx] = ' ';
            draw_cell(cx, cy);
        }
        break;
    default:
        if (c < 32)
            return;
        cells[cy * cols + cx] = c;
        draw_cell(cx, cy);
        cx++;
        break;
    }

    if (cx >= cols) {
        cx = 0;
        cy++;
    }

    if (cy >= rows)
        scroll();

    /* Push on line boundaries rather than per character. Presenting after
     * every glyph would copy the dirty region hundreds of times per line. */
    if (c == '\n')
        fb_present();
}

void fbcon_flush(void)
{
    if (ready)
        fb_present();
}

void fbcon_clear(void)
{
    if (!ready)
        return;

    memset(cells, ' ', (uint32_t)(cols * rows));
    cx = cy = 0;

    fb_clear(colour_bg);
    fb_present();
}

void fbcon_home(void)
{
    cx = cy = 0;
}
