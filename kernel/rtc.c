#include "rtc.h"
#include "io.h"
#include "console.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECOND  0x00
#define RTC_MINUTE  0x02
#define RTC_HOUR    0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_BINARY   0x04
#define STATUS_B_24HOUR   0x02

static bool binary_mode;
static bool hour24_mode;

static uint8_t cmos_read(uint8_t reg)
{
    /* Bit 7 of the index port also controls the non-maskable interrupt. It is
     * left set, disabling NMI for the duration of the access, which is what
     * every other CMOS driver does: an NMI arriving between the index write
     * and the data read would leave the chip pointing somewhere unexpected. */
    outb(CMOS_INDEX, (uint8_t)(0x80 | reg));
    return inb(CMOS_DATA);
}

static bool updating(void)
{
    return (cmos_read(RTC_STATUS_A) & STATUS_A_UPDATING) != 0;
}

/* Binary-coded decimal: each nibble holds one decimal digit, so 0x23 means
 * twenty-three rather than thirty-five. */
static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

void rtc_init(void)
{
    uint8_t status_b = cmos_read(RTC_STATUS_B);

    binary_mode = (status_b & STATUS_B_BINARY) != 0;
    hour24_mode = (status_b & STATUS_B_24HOUR) != 0;

    rtc_time_t now;
    rtc_read(&now);

    kprintf("rtc              : %04u-%02u-%02u %02u:%02u:%02u (%s, %s)\n",
            now.year, now.month, now.day, now.hour, now.minute, now.second,
            binary_mode ? "binary" : "bcd",
            hour24_mode ? "24-hour" : "12-hour");
}

void rtc_read(rtc_time_t *out)
{
    if (!out)
        return;

    /* The chip updates its registers once a second, and reading during that
     * window can return a mix of old and new values -- 12:59:59 becoming
     * 12:00:59. Waiting for the update flag to clear, then reading twice and
     * accepting the result only when both agree, avoids catching it mid-tick. */
    uint8_t sec, min, hour, day, month, year;
    uint8_t p_sec, p_min, p_hour, p_day, p_month, p_year;

    int guard = 0;

    do {
        while (updating() && ++guard < 1000000)
            ;

        sec   = cmos_read(RTC_SECOND);
        min   = cmos_read(RTC_MINUTE);
        hour  = cmos_read(RTC_HOUR);
        day   = cmos_read(RTC_DAY);
        month = cmos_read(RTC_MONTH);
        year  = cmos_read(RTC_YEAR);

        while (updating() && ++guard < 1000000)
            ;

        p_sec   = cmos_read(RTC_SECOND);
        p_min   = cmos_read(RTC_MINUTE);
        p_hour  = cmos_read(RTC_HOUR);
        p_day   = cmos_read(RTC_DAY);
        p_month = cmos_read(RTC_MONTH);
        p_year  = cmos_read(RTC_YEAR);

    } while ((sec != p_sec || min != p_min || hour != p_hour ||
              day != p_day || month != p_month || year != p_year) &&
             guard < 1000000);

    if (!binary_mode) {
        /* The high bit of the hour is the PM flag in 12-hour mode, so it has
         * to come off before the BCD conversion or it corrupts the digit. */
        bool pm = !hour24_mode && (hour & 0x80);

        sec   = from_bcd(sec);
        min   = from_bcd(min);
        hour  = from_bcd((uint8_t)(hour & 0x7F));
        day   = from_bcd(day);
        month = from_bcd(month);
        year  = from_bcd(year);

        if (pm && hour < 12)
            hour = (uint8_t)(hour + 12);
        if (!pm && !hour24_mode && hour == 12)
            hour = 0;
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;

    /* The year register holds two digits. There is a century register in
     * later chipsets but it is not reliably present, so the window is
     * assumed: values below 70 are this century. */
    out->year = (uint16_t)(year < 70 ? 2000 + year : 1900 + year);
}

const char *rtc_month_name(uint8_t month)
{
    static const char *names[13] = {
        "?", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    return names[(month >= 1 && month <= 12) ? month : 0];
}
