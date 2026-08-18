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

/* ---- anti-aliased text ----------------------------------------------------
 *
 * The atlases in font_atlas.c store coverage rather than on/off pixels: how
 * much of each pixel the glyph covers, 0 to 255. Drawing a glyph therefore
 * means blending the text colour into what is already there in proportion to
 * that coverage, which is what puts soft grey along the diagonal of an 'A'
 * instead of a staircase.
 *
 * That is the whole difference between text that looks like a modern desktop
 * and text that looks like a terminal from 1985. It costs one multiply per
 * channel per pixel, which is why the render loop had to stop repainting the
 * screen on every mouse movement first -- doing this work on every frame
 * would have been slower than the bitmap font it replaces.
 */

extern const unsigned char font_mono[95][176];
extern const unsigned char font_mono_adv[95];
extern const int font_mono_w, font_mono_h;

extern const unsigned char font_ui[95][288];
extern const unsigned char font_ui_adv[95];
extern const int font_ui_w, font_ui_h;

/* Blend src over dst by coverage. The divide by 255 is approximated with a
 * shift: exact division per channel per pixel is not worth it when the error
 * is under one part in 256 and invisible. */
static uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
    if (a == 0)   return dst;
    if (a >= 255) return src;

    uint32_t inv = 255 - a;

    uint32_t dr = (dst >> r_shift) & 0xFF;
    uint32_t dg = (dst >> g_shift) & 0xFF;
    uint32_t db = (dst >> b_shift) & 0xFF;

    uint32_t sr = (src >> r_shift) & 0xFF;
    uint32_t sg = (src >> g_shift) & 0xFF;
    uint32_t sb = (src >> b_shift) & 0xFF;

    uint32_t r = (sr * a + dr * inv + 128) >> 8;
    uint32_t g = (sg * a + dg * inv + 128) >> 8;
    uint32_t b = (sb * a + db * inv + 128) >> 8;

    return (r << r_shift) | (g << g_shift) | (b << b_shift);
}

void fb_blend_rect(int x, int y, int w, int h, uint32_t colour, uint32_t alpha)
{
    if (!ready || alpha == 0)
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)width  ? (int)width  : x + w;
    int y1 = y + h > (int)height ? (int)height : y + h;

    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &back[yy * (int)width];
        for (int xx = x0; xx < x1; xx++)
            row[xx] = blend(row[xx], colour, alpha);
    }

    fb_mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

int fb_char_aa(int x, int y, char c, uint32_t colour, bool ui)
{
    if (!ready)
        return 0;

    if (c < 32 || c > 126)
        c = '?';

    int idx = c - 32;

    const unsigned char *glyph = ui ? font_ui[idx]  : font_mono[idx];
    int cw   = ui ? font_ui_w   : font_mono_w;
    int ch   = ui ? font_ui_h   : font_mono_h;
    int adv  = ui ? font_ui_adv[idx] : font_mono_adv[idx];

    for (int row = 0; row < ch; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= (int)height)
            continue;

        uint32_t *line = &back[yy * (int)width];
        const unsigned char *cov = &glyph[row * cw];

        for (int col = 0; col < cw; col++) {
            uint32_t a = cov[col];
            if (a == 0)
                continue;           /* most of a glyph cell is empty */

            int xx = x + col;
            if (xx >= 0 && xx < (int)width)
                line[xx] = blend(line[xx], colour, a);
        }
    }

    fb_mark_dirty(x, y, cw, ch);
    return adv;
}

int fb_text_aa(int x, int y, const char *s, uint32_t colour, bool ui)
{
    int start = x;

    while (*s)
        x += fb_char_aa(x, y, *s++, colour, ui);

    return x - start;
}

int fb_text_width(const char *s, bool ui)
{
    int w = 0;

    while (*s) {
        char c = *s++;
        if (c < 32 || c > 126)
            c = '?';
        w += ui ? font_ui_adv[c - 32] : font_mono_adv[c - 32];
    }

    return w;
}

int fb_font_height(bool ui) { return ui ? font_ui_h : font_mono_h; }
int fb_mono_advance(void)   { return font_mono_adv['M' - 32]; }

/* ---- rounded rectangles and shadows ---------------------------------------
 *
 * Both are alpha work rather than shape work. A rounded corner is a filled
 * rectangle whose corner pixels are blended by how much of each lies inside
 * the curve, and a drop shadow is several translucent rectangles stacked with
 * falling opacity. Doing the corners by simply omitting pixels gives a hard
 * stair-stepped arc that looks worse than a square corner.
 */

/* Coverage of a pixel by a quarter circle, approximated by sampling the pixel
 * at four points. Exact area would need square roots; four samples give five
 * levels of coverage, which at these radii is indistinguishable. */
static uint32_t corner_coverage(int px, int py, int r)
{
    int inside = 0;

    for (int sy = 0; sy < 2; sy++) {
        for (int sx = 0; sx < 2; sx++) {
            int dx = r - (px * 2 + sx) / 2 - 1;
            int dy = r - (py * 2 + sy) / 2 - 1;

            if (dx * dx + dy * dy <= r * r)
                inside++;
        }
    }

    return (uint32_t)(inside * 255 / 4);
}

void fb_fill_round_rect(int x, int y, int w, int h, int r, uint32_t colour)
{
    if (!ready || w <= 0 || h <= 0)
        return;

    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 1) {
        fb_fill_rect(x, y, w, h, colour);
        return;
    }

    /* Middle band full width, then the two end bands inset by the radius. */
    fb_fill_rect(x, y + r, w, h - 2 * r, colour);
    fb_fill_rect(x + r, y, w - 2 * r, r, colour);
    fb_fill_rect(x + r, y + h - r, w - 2 * r, r, colour);

    for (int py = 0; py < r; py++) {
        for (int px = 0; px < r; px++) {
            uint32_t a = corner_coverage(px, py, r);
            if (a == 0)
                continue;

            fb_blend_rect(x + px,             y + py,             1, 1, colour, a);
            fb_blend_rect(x + w - 1 - px,     y + py,             1, 1, colour, a);
            fb_blend_rect(x + px,             y + h - 1 - py,     1, 1, colour, a);
            fb_blend_rect(x + w - 1 - px,     y + h - 1 - py,     1, 1, colour, a);
        }
    }
}

/* Blend a rounded rectangle rather than filling it solid. Needed for the
 * shadow, which is built from many faint layers. */
void fb_blend_round_rect(int x, int y, int w, int h, int r,
                         uint32_t colour, uint32_t alpha)
{
    if (!ready || w <= 0 || h <= 0 || alpha == 0)
        return;

    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    if (r < 1) {
        fb_blend_rect(x, y, w, h, colour, alpha);
        return;
    }

    fb_blend_rect(x, y + r, w, h - 2 * r, colour, alpha);
    fb_blend_rect(x + r, y, w - 2 * r, r, colour, alpha);
    fb_blend_rect(x + r, y + h - r, w - 2 * r, r, colour, alpha);

    for (int py = 0; py < r; py++) {
        for (int px = 0; px < r; px++) {
            uint32_t cov = corner_coverage(px, py, r);
            if (cov == 0)
                continue;

            /* Corner coverage scales the layer's own opacity, so a partly
             * covered corner pixel in a faint layer stays faint. */
            uint32_t a = (cov * alpha) >> 8;
            if (a == 0)
                continue;

            fb_blend_rect(x + px,         y + py,         1, 1, colour, a);
            fb_blend_rect(x + w - 1 - px, y + py,         1, 1, colour, a);
            fb_blend_rect(x + px,         y + h - 1 - py, 1, 1, colour, a);
            fb_blend_rect(x + w - 1 - px, y + h - 1 - py, 1, 1, colour, a);
        }
    }
}

void fb_shadow(int x, int y, int w, int h, int r, int spread)
{
    if (!ready || spread <= 0)
        return;

    uint32_t black = fb_rgb(0, 0, 0);

    /* Faint layers, largest first, each one slightly smaller than the last.
     * Where they overlap the opacity accumulates, so the result is dark near
     * the window and fades outwards -- a gradient built from flat fills.
     *
     * The offset downward is what makes it read as a shadow rather than a
     * halo: light is assumed to come from above, as it does in every desktop
     * interface.
     */
    for (int i = spread; i >= 1; i--)
        fb_blend_round_rect(x - i, y - i + 3, w + i * 2, h + i * 2,
                            r + i, black, 10);
}
