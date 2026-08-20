#include "wallpaper.h"
#include "wallpaper_image.h"
#include "fb.h"

static int current;

static const char *names[WALLPAPER_COUNT] = {
    "Picture", "Midnight", "Slate", "Dusk", "Deep Sea", "Charcoal"
};

/* Style 0 is the embedded picture; the rest are generated. Keeping the
 * generated ones means the desktop still has a background on a build where no
 * image was supplied, and gives something to switch to. */
#define STYLE_PICTURE 0
#define GRADIENT_COUNT (WALLPAPER_COUNT - 1)

/* Each style is a pair of colours interpolated down the screen, drawn as
 * bands. More bands means a smoother ramp; 96 is past the point where the
 * steps are visible at these sizes, and it is still only 96 fills. */
typedef struct {
    uint8_t r0, g0, b0;     /* top */
    uint8_t r1, g1, b1;     /* bottom */
    bool    vignette;
} style_t;

static const style_t styles[GRADIENT_COUNT] = {
    {  22,  30,  48,    8,  12,  22, true  },   /* Midnight */
    {  46,  52,  64,   26,  30,  38, false },   /* Slate    */
    {  58,  42,  72,   20,  18,  38, true  },   /* Dusk     */
    {  14,  46,  62,    6,  18,  30, true  },   /* Deep Sea */
    {  38,  38,  40,   18,  18,  20, false },   /* Charcoal */
};

/* The palette as framebuffer words, converted once. Doing it per pixel would
 * be two million conversions for a single repaint. */
static uint32_t palette[256];
static bool     palette_ready;

/* Horizontal source column for each screen column. The mapping depends only
 * on the two widths, so it is computed when either changes rather than once
 * per pixel -- a divide per pixel is two million divides a repaint. */
#define MAX_SCREEN_W 2560

static int xmap[MAX_SCREEN_W];
static int xmap_for_w;

static uint32_t row_buf[MAX_SCREEN_W];

/* Draw the part of the wallpaper inside the clip rectangle.
 *
 * A repaint that covers the whole desktop is two million palette lookups and
 * eight megabytes of copying. Most repaints do not need the whole desktop:
 * when a window moves, only the strip it uncovered has changed. Restricting
 * the work to that strip is what makes dragging cost a few hundred kilobytes
 * instead of the entire screen.
 */
static void draw_picture(int w, int h, int cx, int cy, int cw, int ch)
{
    int iw = wallpaper_image_w;
    int ih = wallpaper_image_h;

    if (!palette_ready) {
        for (int i = 0; i < 256; i++)
            palette[i] = fb_rgb(wallpaper_image_palette[i * 3 + 0],
                                wallpaper_image_palette[i * 3 + 1],
                                wallpaper_image_palette[i * 3 + 2]);
        palette_ready = true;
    }

    if (xmap_for_w != w) {
        for (int x = 0; x < w && x < MAX_SCREEN_W; x++)
            xmap[x] = (x * iw) / w;
        xmap_for_w = w;
    }

    if (w > MAX_SCREEN_W)
        w = MAX_SCREEN_W;

    /* Nearest neighbour. The picture is stored at half the screen dimensions,
     * so in the usual case every source pixel simply becomes a 2x2 block, and
     * a filter would cost time to soften something already being enlarged. */
    for (int y = cy; y < cy + ch; y++) {
        if (y < 0 || y >= h)
            continue;

        const uint8_t *src = wallpaper_image_pixels + (y * ih / h) * iw;

        for (int x = cx; x < cx + cw; x++)
            row_buf[x - cx] = palette[src[xmap[x]]];

        fb_write_rect(cx, y, cw, 1, row_buf);
    }
}

void wallpaper_draw(int w, int h)
{
    wallpaper_draw_clip(w, h, 0, 0, w, h);
}

void wallpaper_draw_clip(int w, int h, int cx, int cy, int cw, int ch)
{
    /* Clamp the clip to the desktop, so a caller can pass a window rectangle
     * that hangs off the edge without checking first. */
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > w) cw = w - cx;
    if (cy + ch > h) ch = h - cy;

    if (cw <= 0 || ch <= 0)
        return;

    /* An unsupplied picture is not an error: the build works without one, and
     * the desktop falls back to the first gradient. */
    if (current == STYLE_PICTURE) {
        if (wallpaper_image_w > 0 && wallpaper_image_h > 0) {
            draw_picture(w, h, cx, cy, cw, ch);
            return;
        }

        current = 1;
    }

    const style_t *s = &styles[current - 1];

    int bands = 96;
    int bh = (h + bands - 1) / bands;

    for (int i = 0; i < bands; i++) {
        int by = i * bh;

        /* Skip bands that fall outside the clip entirely. */
        if (by + bh <= cy || by >= cy + ch)
            continue;

        /* Linear interpolation in integer arithmetic: no floating point is
         * available, and none is needed for 96 steps. */
        int r = s->r0 + (s->r1 - s->r0) * i / bands;
        int g = s->g0 + (s->g1 - s->g0) * i / bands;
        int b = s->b0 + (s->b1 - s->b0) * i / bands;

        int y0 = by > cy ? by : cy;
        int y1 = (by + bh) < (cy + ch) ? (by + bh) : (cy + ch);

        fb_fill_rect(cx, y0, cw, y1 - y0,
                     fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }

    if (!s->vignette)
        return;

    /* A soft darkening towards the edges. Real wallpapers rarely have uniform
     * brightness corner to corner, and a slight falloff stops a flat gradient
     * reading as a painted wall. */
    uint32_t black = fb_rgb(0, 0, 0);
    int steps = 12;

    for (int i = 0; i < steps; i++) {
        int inset = i * (w / 60);
        uint32_t a = (uint32_t)(steps - i);

        /* Each band is intersected with the clip rather than drawn whole:
         * blending outside it would darken pixels a second time. */
        int lx = 0, lw = inset;
        if (lx < cx) { lw -= cx - lx; lx = cx; }
        if (lx + lw > cx + cw) lw = cx + cw - lx;
        if (lw > 0)
            fb_blend_rect(lx, cy, lw, ch, black, a);

        int rx = w - inset, rw = inset;
        if (rx < cx) { rw -= cx - rx; rx = cx; }
        if (rx + rw > cx + cw) rw = cx + cw - rx;
        if (rw > 0)
            fb_blend_rect(rx, cy, rw, ch, black, a);
    }
}

void wallpaper_next(void)
{
    current = (current + 1) % WALLPAPER_COUNT;
}

void wallpaper_set(int index)
{
    if (index >= 0 && index < WALLPAPER_COUNT)
        current = index;
}

int         wallpaper_current(void)    { return current; }
const char *wallpaper_name(int index)
{
    if (index < 0 || index >= WALLPAPER_COUNT)
        return "?";
    return names[index];
}
