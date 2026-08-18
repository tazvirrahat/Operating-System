p='kernel/gui.c'
s=open(p).read()

s=s.replace('#include "pit.h"','#include "pit.h"\n#include "rtc.h"')

# --- 1. wheel scrolls the terminal, when the pointer is over it ---
s=s.replace('''        if (mouse_x() != last_mx || mouse_y() != last_my) {''',
'''        /* Wheel scrolls whichever window the pointer is over, which for now
         * means the terminal. Scrolling the window under the pointer rather
         * than the focused one is what every desktop does, and it avoids
         * having to click a window before it will respond. */
        int wheel = mouse_take_wheel();
        if (wheel != 0) {
            for (int i = window_count - 1; i >= 0; i--) {
                if (!windows[i].visible || windows[i].kind != WIN_TERMINAL)
                    continue;
                if (!in_rect(mouse_x(), mouse_y(), windows[i].x, windows[i].y,
                             windows[i].w, windows[i].h))
                    continue;

                term_scroll_view(wheel * 3);
                dirty = true;
                scene_dirty = true;
                break;
            }
        }

        if (mouse_x() != last_mx || mouse_y() != last_my) {''')

# --- 2. clock and date on the taskbar, replacing the uptime counter ---
old_clock = s[s.index('''    /* Uptime where a clock would be. There is no real-time clock driver, so
     * showing a wall clock would mean inventing one. */'''):s.index('static void draw_cursor')]

new_clock = '''    /* Real wall-clock time and date, read from the CMOS chip. The PIT can
     * only say how long the machine has been up; the date has to come from
     * hardware that keeps counting while it is switched off. */
    rtc_time_t now;
    rtc_read(&now);

    char clock[24];
    int  n = 0;

    clock[n++] = (char)('0' + now.hour / 10);
    clock[n++] = (char)('0' + now.hour % 10);
    clock[n++] = ':';
    clock[n++] = (char)('0' + now.minute / 10);
    clock[n++] = (char)('0' + now.minute % 10);
    clock[n]   = 0;

    char date[24];
    n = 0;

    date[n++] = (char)('0' + now.day / 10);
    date[n++] = (char)('0' + now.day % 10);
    date[n++] = ' ';

    const char *mon = rtc_month_name(now.month);
    while (*mon)
        date[n++] = *mon++;

    date[n++] = ' ';
    date[n++] = (char)('0' + (now.year / 1000) % 10);
    date[n++] = (char)('0' + (now.year / 100) % 10);
    date[n++] = (char)('0' + (now.year / 10) % 10);
    date[n++] = (char)('0' + now.year % 10);
    date[n]   = 0;

    /* Right-aligned, measured rather than assumed: the interface face is
     * proportional, so the width of "18 Aug 2026" is not a character count. */
    int cw = fb_text_width(clock, true);
    int dw = fb_text_width(date, true);
    int wide = cw > dw ? cw : dw;

    int cx = SCR_W - wide - 18;
    int line_h = fb_font_height(true);

    fb_text_aa(cx + (wide - cw), bar_y + (TASKBAR_H / 2) - line_h + 2,
               clock, col_white, true);
    fb_text_aa(cx + (wide - dw), bar_y + (TASKBAR_H / 2) + 2,
               date, col_accent, true);
}

'''
s = s.replace(old_clock, new_clock)

# the taskbar button width reserved space for the old uptime counter
s = s.replace('int clock_w = 9 * CHROME_W;', 'int clock_w = 12 * CHROME_W;')

# --- 3. wallpaper: a vertical gradient instead of a flat fill ---
s=s.replace('''            fb_fill_rect(0, 0, SCR_W, SCR_H - TASKBAR_H, col_desktop);''',
'''            /* A vertical gradient rather than a flat colour. Two fills and a
             * blend per band; the eye reads the result as depth, and it is
             * the cheapest thing that stops a desktop looking like a slab of
             * one colour. */
            {
                int h = SCR_H - TASKBAR_H;
                int bands = 48;
                int band_h = (h + bands - 1) / bands;

                for (int i = 0; i < bands; i++) {
                    uint32_t a = (uint32_t)(i * 90 / bands);
                    fb_fill_rect(0, i * band_h, SCR_W, band_h, col_desktop);
                    fb_blend_rect(0, i * band_h, SCR_W, band_h, col_black, a);
                }
            }''')

open(p,'w').write(s)
print("ok")
