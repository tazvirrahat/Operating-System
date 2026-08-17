#include "gfx.h"
#include "io.h"
#include "string.h"
#include "vga.h"

/* VGA register ports. */
#define VGA_MISC_WRITE  0x3C2
#define VGA_SEQ_INDEX   0x3C4
#define VGA_SEQ_DATA    0x3C5
#define VGA_GC_INDEX    0x3CE
#define VGA_GC_DATA     0x3CF
#define VGA_CRTC_INDEX  0x3D4
#define VGA_CRTC_DATA   0x3D5
#define VGA_AC_INDEX    0x3C0
#define VGA_AC_WRITE    0x3C0
#define VGA_INSTAT_READ 0x3DA
#define VGA_DAC_INDEX_W 0x3C8
#define VGA_DAC_DATA    0x3C9

#define FRAMEBUFFER ((volatile uint8_t *)0xA0000)

/* Off-screen buffer. 64 KB in BSS; drawing here and copying once avoids the
 * tearing that comes from writing video memory while the display reads it. */
static uint8_t backbuffer[GFX_WIDTH * GFX_HEIGHT];

static bool active;

/* ---- mode register tables ------------------------------------------------
 *
 * These are the documented register values for the two modes. They are opaque
 * by nature: each byte configures timing, addressing or attribute behaviour in
 * the CRT controller, and there is no way to make the table self-explanatory.
 * They come from the standard VGA mode tables.
 */

static const uint8_t mode13_seq[5]  = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const uint8_t mode13_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF
};
static const uint8_t mode13_gc[9]   = { 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x40, 0x05, 0x0F, 0xFF };
static const uint8_t mode13_ac[21]  = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

static const uint8_t text3_seq[5]   = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t text3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};
static const uint8_t text3_gc[9]    = { 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x10, 0x0E, 0x00, 0xFF };
static const uint8_t text3_ac[21]   = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static void write_registers(uint8_t misc,
                            const uint8_t *seq,
                            const uint8_t *crtc,
                            const uint8_t *gc,
                            const uint8_t *ac)
{
    outb(VGA_MISC_WRITE, misc);

    for (uint8_t i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, seq[i]);
    }

    /* CRTC registers 0-7 are write-protected by the high bit of register 0x11.
     * Clearing it first is mandatory; without it the first eight values are
     * silently ignored and the display timing stays wrong. */
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & 0x7F));

    for (uint8_t i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, gc[i]);
    }

    /* The attribute controller uses one port as both index and data, toggled
     * by an internal flip-flop. Reading the input status register resets that
     * flip-flop to the index state, which is why it appears in the loop. */
    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT_READ);
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_WRITE, ac[i]);
    }

    /* Bit 5 re-enables video output, which the sequence above disabled. */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

/* ---- text-mode font preservation -----------------------------------------
 *
 * Mode 13h uses the whole of video memory for pixel data, which destroys the
 * character glyphs the text mode keeps in plane 2. Without saving them first,
 * returning to text mode gives a screen of garbage: the timing is right, the
 * characters are correct, and every glyph is whatever pixel data happened to
 * land there.
 */

#define FONT_BYTES (256 * 32)
static uint8_t saved_font[FONT_BYTES];

static void font_access_begin(void)
{
    /* Address plane 2 directly, with the odd/even and chain-4 addressing
     * modes turned off so the font bytes are linearly addressable. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04);  /* write plane 2 */
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x07);  /* no odd/even, no chain-4 */
    outb(VGA_GC_INDEX,  0x04); outb(VGA_GC_DATA,  0x02);  /* read plane 2 */
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  0x00);  /* no odd/even */
    outb(VGA_GC_INDEX,  0x06); outb(VGA_GC_DATA,  0x04);  /* map at 0xA0000 */
}

static void font_access_end(void)
{
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x03);
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x03);
    outb(VGA_GC_INDEX,  0x04); outb(VGA_GC_DATA,  0x00);
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  0x10);
    outb(VGA_GC_INDEX,  0x06); outb(VGA_GC_DATA,  0x0E);
}

static void font_save(void)
{
    font_access_begin();
    memcpy(saved_font, (const void *)0xA0000, FONT_BYTES);
    font_access_end();
}

static void font_restore(void)
{
    font_access_begin();
    memcpy((void *)0xA0000, saved_font, FONT_BYTES);
    font_access_end();
}

/* ---- palette ------------------------------------------------------------- */

static void set_colour(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    /* The DAC takes 6-bit components, so values are 0-63 rather than 0-255. */
    outb(VGA_DAC_INDEX_W, index);
    outb(VGA_DAC_DATA, (uint8_t)(r >> 2));
    outb(VGA_DAC_DATA, (uint8_t)(g >> 2));
    outb(VGA_DAC_DATA, (uint8_t)(b >> 2));
}

static void init_palette(void)
{
    set_colour(C_DESKTOP,    40,  70, 110);
    set_colour(C_WIN_FACE,  190, 190, 190);
    set_colour(C_WIN_EDGE,  245, 245, 245);
    set_colour(C_WIN_SHADE,  90,  90,  90);
    set_colour(C_TITLE_ON,   30,  80, 160);
    set_colour(C_TITLE_OFF, 120, 120, 120);
    set_colour(C_TERM_BG,    15,  15,  25);
}

/* ---- mode switching ------------------------------------------------------ */

void gfx_enter(void)
{
    if (active)
        return;

    font_save();

    write_registers(0x63, mode13_seq, mode13_crtc, mode13_gc, mode13_ac);
    init_palette();

    memset(backbuffer, C_BLACK, sizeof(backbuffer));
    active = true;

    gfx_present();
}

void gfx_leave(void)
{
    if (!active)
        return;

    write_registers(0x67, text3_seq, text3_crtc, text3_gc, text3_ac);
    font_restore();

    active = false;

    /* Repaint from the text driver's own state, which was untouched. */
    vga_clear();
}

bool gfx_active(void)
{
    return active;
}

/* ---- drawing ------------------------------------------------------------- */

void gfx_present(void)
{
    memcpy((void *)FRAMEBUFFER, backbuffer, sizeof(backbuffer));
}

void gfx_clear(uint8_t colour)
{
    memset(backbuffer, colour, sizeof(backbuffer));
}

void gfx_pixel(int x, int y, uint8_t colour)
{
    if (x < 0 || y < 0 || x >= GFX_WIDTH || y >= GFX_HEIGHT)
        return;

    backbuffer[y * GFX_WIDTH + x] = colour;
}

void gfx_hline(int x, int y, int w, uint8_t colour)
{
    if (y < 0 || y >= GFX_HEIGHT)
        return;

    if (x < 0) { w += x; x = 0; }
    if (x + w > GFX_WIDTH) w = GFX_WIDTH - x;
    if (w <= 0) return;

    memset(&backbuffer[y * GFX_WIDTH + x], colour, (uint32_t)w);
}

void gfx_vline(int x, int y, int h, uint8_t colour)
{
    for (int i = 0; i < h; i++)
        gfx_pixel(x, y + i, colour);
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t colour)
{
    for (int i = 0; i < h; i++)
        gfx_hline(x, y + i, w, colour);
}

void gfx_rect(int x, int y, int w, int h, uint8_t colour)
{
    gfx_hline(x, y, w, colour);
    gfx_hline(x, y + h - 1, w, colour);
    gfx_vline(x, y, h, colour);
    gfx_vline(x + w - 1, y, h, colour);
}

/* ---- text ---------------------------------------------------------------- */

/* Defined in font8x8.c. Bits are least-significant-first: bit 0 is the
 * leftmost pixel of the row. */
extern const uint8_t font8x8[95][8];

void gfx_char(int x, int y, char c, uint8_t fg)
{
    if (c < 32 || c > 126)
        c = '?';

    const uint8_t *glyph = font8x8[c - 32];

    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (1 << col))
                gfx_pixel(x + col, y + row, fg);
        }
    }
}

void gfx_text(int x, int y, const char *s, uint8_t fg)
{
    while (*s) {
        gfx_char(x, y, *s++, fg);
        x += GLYPH_W;
    }
}
