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

static void draw_picture(int w, int h)
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
    for (int y = 0; y < h; y++) {
        const uint8_t *src = wallpaper_image_pixels + (y * ih / h) * iw;

        for (int x = 0; x < w; x++)
            row_buf[x] = palette[src[xmap[x]]];

        fb_write_rect(0, y, w, 1, row_buf);
    }
}

void wallpaper_draw(int w, int h)
{
    /* An unsupplied picture is not an error: the build works without one, and
     * the desktop falls back to the first gradient. */
    if (current == STYLE_PICTURE) {
        if (wallpaper_image_w > 0 && wallpaper_image_h > 0) {
            draw_picture(w, h);
            return;
        }

        current = 1;
    }

    const style_t *s = &styles[current - 1];

    int bands = 96;
    int bh = (h + bands - 1) / bands;

    for (int i = 0; i < bands; i++) {
        /* Linear interpolation in integer arithmetic: no floating point is
         * available, and none is needed for 96 steps. */
        int r = s->r0 + (s->r1 - s->r0) * i / bands;
        int g = s->g0 + (s->g1 - s->g0) * i / bands;
        int b = s->b0 + (s->b1 - s->b0) * i / bands;

        fb_fill_rect(0, i * bh, w, bh, fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
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

        fb_blend_rect(0, 0, inset, h, black, a);
        fb_blend_rect(w - inset, 0, inset, h, black, a);
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
