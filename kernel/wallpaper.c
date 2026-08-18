#include "wallpaper.h"
#include "fb.h"

static int current;

static const char *names[WALLPAPER_COUNT] = {
    "Midnight", "Slate", "Dusk", "Deep Sea", "Charcoal"
};

/* Each style is a pair of colours interpolated down the screen, drawn as
 * bands. More bands means a smoother ramp; 96 is past the point where the
 * steps are visible at these sizes, and it is still only 96 fills. */
typedef struct {
    uint8_t r0, g0, b0;     /* top */
    uint8_t r1, g1, b1;     /* bottom */
    bool    vignette;
} style_t;

static const style_t styles[WALLPAPER_COUNT] = {
    {  22,  30,  48,    8,  12,  22, true  },   /* Midnight */
    {  46,  52,  64,   26,  30,  38, false },   /* Slate    */
    {  58,  42,  72,   20,  18,  38, true  },   /* Dusk     */
    {  14,  46,  62,    6,  18,  30, true  },   /* Deep Sea */
    {  38,  38,  40,   18,  18,  20, false },   /* Charcoal */
};

void wallpaper_draw(int w, int h)
{
    const style_t *s = &styles[current];

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
