/* rtc.h — the CMOS real-time clock.
 *
 * Until now the kernel has had no idea what time it is. The PIT counts ticks
 * since boot, which gives uptime but says nothing about the date: it starts
 * at zero every time the machine starts. Wall-clock time comes from a
 * separate battery-backed chip that keeps counting while the machine is off.
 *
 * It is read through the same index/data port pair as the rest of CMOS, and
 * the values are usually in binary-coded decimal rather than binary -- the
 * hour 23 arrives as 0x23, not 0x17.
 */
#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  second, minute, hour;
    uint8_t  day, month;
    uint16_t year;
} rtc_time_t;

void rtc_init(void);
void rtc_read(rtc_time_t *out);

/* Name of the month, 1-12. */
const char *rtc_month_name(uint8_t month);

#endif /* RTC_H */
