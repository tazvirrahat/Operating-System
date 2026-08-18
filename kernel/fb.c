#include "fb.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "svga.h"

extern const uint8_t font8x8[95][8];

static volatile uint8_t *video;     /* the real framebuffer, in video memory */
static uint32_t         *back;      /* our copy, in ordinary RAM */

static uint32_t width, height, pitch;
static uint8_t  bpp;
static bool     ready;

/* Component positions, taken from what GRUB reports rather than assumed.
 * 32-bit modes are usually BGRX, but that is a convention, not a guarantee. */
static uint8_t r_shift = 16, g_shift = 8, b_shift = 0;

/* Bounding box of everything drawn since the last present. Tracking one
 * rectangle rather than a list keeps this simple; the cost is that two small
 * changes at opposite corners force a large copy, which in practice is rare
 * because the mouse and the focused window are usually near each other. */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static bool dirty;

bool fb_init(const multiboot_info_t *mb)
{
    if (!mb || !(mb->flags & MB_INFO_FRAMEBUFFER))
        return false;

    if (mb->framebuffer_type != MB_FRAMEBUFFER_RGB || mb->framebuffer_bpp != 32)
        return false;

    /* The framebuffer can sit anywhere in the physical address space, often
     * well above installed RAM. It is only usable because paging identity-maps
     * that range; a mapping restricted to reported memory would exclude it. */
    video  = (volatile uint8_t *)(uint32_t)mb->framebuffer_addr;
    width  = mb->framebuffer_width;
    height = mb->framebuffer_height;
    pitch  = mb->framebuffer_pitch;
    bpp    = mb->framebuffer_bpp;

    /* Channel positions, but only if they describe a layout that actually
     * exists. Every 32-bit mode in practice is one of two arrangements:
     *
     *   BGRX   blue at bit 0,  green at 8, red at 16   (overwhelmingly common)
     *   RGBX   red at bit 0,   green at 8, blue at 16
     *
     * Anything else is a misreport. This is not hypothetical: under QEMU the
     * values arrive as red=0, green=16, blue=8, which is neither layout. Using
     * them tinted the whole display green — a dark blue written through that
     * mapping lands in memory as bytes the hardware reads back as green.
     *
     * A plain range check does not catch it, since those numbers are a valid
     * permutation of the right set. Matching against the real layouts does. */
    uint8_t r = mb->red_position;
    uint8_t g = mb->green_position;
    uint8_t b = mb->blue_position;

    bool bgrx = (r == 16 && g == 8 && b == 0);
    bool rgbx = (r == 0  && g == 8 && b == 16);

    if (bgrx || rgbx) {
        r_shift = r;
        g_shift = g;
        b_shift = b;
    } else {
        r_shift = 16;
        g_shift = 8;
        b_shift = 0;
    }

    back = kmalloc(width * height * 4);
    if (!back) {
        video = 0;
        return false;
    }

    memset(back, 0, width * height * 4);

    ready = true;
    fb_mark_all_dirty();

    kprintf("framebuffer      : %ux%u %u bpp, pitch %u, %u KB backbuffer\n",
            width, height, bpp, pitch, (width * height * 4) / 1024);
    kprintf("                 : pixel layout r=%u g=%u b=%u%s\n",
            r_shift, g_shift, b_shift,
            (bgrx || rgbx) ? "" : " (reported layout invalid, assuming BGRX)");

    return true;
}

bool     fb_available(void) { return ready; }
uint32_t fb_width(void)     { return width; }
uint32_t fb_height(void)    { return height; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << r_shift)
         | ((uint32_t)g << g_shift)
         | ((uint32_t)b << b_shift);
}

void fb_mark_all_dirty(void)
{
    dirty_x0 = 0;
    dirty_y0 = 0;
    dirty_x1 = (int)width;
    dirty_y1 = (int)height;
    dirty = true;
}

void fb_mark_dirty(int x, int y, int w, int h)
{
    if (!ready || w <= 0 || h <= 0)
        return;

    int x1 = x + w;
    int y1 = y + h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int)width)  x1 = (int)width;
    if (y1 > (int)height) y1 = (int)height;
    if (x >= x1 || y >= y1)
        return;

    if (!dirty) {
        dirty_x0 = x; dirty_y0 = y;
        dirty_x1 = x1; dirty_y1 = y1;
        dirty = true;
        return;
    }

    if (x  < dirty_x0) dirty_x0 = x;
    if (y  < dirty_y0) dirty_y0 = y;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

void fb_present(void)
{
    if (!ready || !dirty)
        return;

    /* Copy row by row. Writes to video memory are uncached and, under a
     * hypervisor, intercepted, so the only thing that matters here is moving
     * as few bytes as possible. */
    int w = dirty_x1 - dirty_x0;

    for (int y = dirty_y0; y < dirty_y1; y++) {
        const uint32_t *src = &back[y * width + dirty_x0];
        volatile uint8_t *dst = video + (uint32_t)y * pitch + (uint32_t)dirty_x0 * 4;

        memcpy((void *)dst, src, (uint32_t)w * 4);
    }

    /* On an SVGA adapter, writing to the framebuffer is not enough on its own:
     * the device has to be told which region changed before it will show it.
     * The bounds are already tracked for the copy above, so the same rectangle
     * is handed to the adapter. */
    if (svga_available())
        svga_update(dirty_x0, dirty_y0, w, dirty_y1 - dirty_y0);

    dirty = false;
}

void fb_clear(uint32_t colour)
{
    if (!ready)
        return;

    for (uint32_t i = 0; i < width * height; i++)
        back[i] = colour;

    fb_mark_all_dirty();
}

void fb_pixel(int x, int y, uint32_t colour)
{
    if (!ready || x < 0 || y < 0 || x >= (int)width || y >= (int)height)
        return;

    back[y * (int)width + x] = colour;
    fb_mark_dirty(x, y, 1, 1);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t colour)
{
    if (!ready)
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)width  ? (int)width  : x + w;
    int y1 = y + h > (int)height ? (int)height : y + h;

    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &back[yy * (int)width];
        for (int xx = x0; xx < x1; xx++)
            row[xx] = colour;
    }

    fb_mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

void fb_rect(int x, int y, int w, int h, uint32_t colour)
{
    fb_fill_rect(x, y, w, 1, colour);
    fb_fill_rect(x, y + h - 1, w, 1, colour);
    fb_fill_rect(x, y, 1, h, colour);
    fb_fill_rect(x + w - 1, y, 1, h, colour);
}

void fb_char(int x, int y, char c, uint32_t fg, int scale)
{
    if (!ready)
        return;

    if (c < 32 || c > 126)
        c = '?';

    if (scale < 1)
        scale = 1;

    const uint8_t *glyph = font8x8[c - 32];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        if (bits == 0)
            continue;

        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col)))
                continue;

            /* Each font pixel becomes a scale x scale block. Written straight
             * into the backbuffer without going through fb_pixel, which would
             * re-check bounds and update the dirty box once per pixel. */
            int px = x + col * scale;
            int py = y + row * scale;

            for (int sy = 0; sy < scale; sy++) {
                int yy = py + sy;
                if (yy < 0 || yy >= (int)height)
                    continue;

                uint32_t *dst = &back[yy * (int)width];

                for (int sx = 0; sx < scale; sx++) {
                    int xx = px + sx;
                    if (xx >= 0 && xx < (int)width)
                        dst[xx] = fg;
                }
            }
        }
    }

    fb_mark_dirty(x, y, 8 * scale, 8 * scale);
}

void fb_text(int x, int y, const char *s, uint32_t fg, int scale)
{
    while (*s) {
        fb_char(x, y, *s++, fg, scale);
        x += 8 * scale;
    }
}

void fb_read_rect(int x, int y, int w, int h, uint32_t *dst)
{
    if (!ready || !dst)
        return;

    for (int row = 0; row < h; row++) {
        int yy = y + row;

        for (int col = 0; col < w; col++) {
            int xx = x + col;

            /* Off-screen pixels read as black rather than being skipped, so
             * the saved block stays rectangular and can be written back with
             * the same geometry. */
            dst[row * w + col] =
                (xx >= 0 && yy >= 0 && xx < (int)width && yy < (int)height)
                ? back[yy * (int)width + xx]
                : 0;
        }
    }
}

void fb_write_rect(int x, int y, int w, int h, const uint32_t *src)
{
    if (!ready || !src)
        return;

    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= (int)height)
            continue;

        uint32_t *line = &back[yy * (int)width];

        for (int col = 0; col < w; col++) {
            int xx = x + col;
            if (xx >= 0 && xx < (int)width)
                line[xx] = src[row * w + col];
        }
    }

    fb_mark_dirty(x, y, w, h);
}
