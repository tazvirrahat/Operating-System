#include "console.h"
#include "vga.h"
#include "serial.h"

#include <stdarg.h>
#include <stdbool.h>

/* stdarg.h is one of the few headers available in a freestanding environment,
 * so variadic functions work without any runtime library. */

void console_init(void)
{
    serial_init();   /* first: this is the channel that survives a crash */
    vga_init();
}

void kputc(char c)
{
    /* A bare newline moves down but not left on a real terminal, so the serial
     * side needs an explicit carriage return. VGA handles '\n' itself. */
    if (c == '\n')
        serial_putc('\r');

    serial_putc(c);
    vga_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

static void print_uint(uint32_t value, uint32_t base, bool upper, int pad)
{
    static const char *lower_digits = "0123456789abcdef";
    static const char *upper_digits = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;

    char buf[32];
    int i = 0;

    if (value == 0)
        buf[i++] = '0';

    while (value > 0) {
        buf[i++] = digits[value % base];
        value /= base;
    }

    while (i < pad)
        buf[i++] = '0';

    while (i-- > 0)
        kputc(buf[i]);
}

static void print_int(int32_t value, int pad)
{
    if (value < 0) {
        kputc('-');
        /* Negating INT32_MIN overflows, so cast before negating. */
        print_uint((uint32_t)(-(int64_t)value), 10, false, pad);
    } else {
        print_uint((uint32_t)value, 10, false, pad);
    }
}

static void vkprintf(const char *fmt, va_list args)
{
    while (*fmt) {
        if (*fmt != '%') {
            kputc(*fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        /* Optional zero-padded width, e.g. %08x. Only '0' fill is supported. */
        int pad = 0;
        if (*fmt == '0') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                pad = pad * 10 + (*fmt++ - '0');
        }

        switch (*fmt) {
        case 'c':
            kputc((char)va_arg(args, int));
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'd':
        case 'i':
            print_int(va_arg(args, int32_t), pad);
            break;
        case 'u':
            print_uint(va_arg(args, uint32_t), 10, false, pad);
            break;
        case 'x':
            print_uint(va_arg(args, uint32_t), 16, false, pad);
            break;
        case 'X':
            print_uint(va_arg(args, uint32_t), 16, true, pad);
            break;
        case 'p':
            kputs("0x");
            print_uint((uint32_t)(uintptr_t)va_arg(args, void *), 16, false, 8);
            break;
        case '%':
            kputc('%');
            break;
        case '\0':
            return;     /* trailing '%' at end of string */
        default:
            kputc('%');
            kputc(*fmt);
            break;
        }

        fmt++;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}

void panic(const char *fmt, ...)
{
    va_list args;

    vga_set_color(VGA_WHITE, VGA_RED);
    kputs("\n*** KERNEL PANIC ***\n");

    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);

    kputs("\nsystem halted.\n");

    /* Disable interrupts and stop. hlt alone can be woken by an NMI, so loop. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}
