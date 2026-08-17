#include "console.h"
#include "vga.h"
#include "serial.h"
#include "task.h"

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

void console_home(void)
{
    /* The VGA driver deliberately does not interpret ANSI escapes — it is a
     * memory-mapped grid, not a terminal — so writing "\033[H" to it would
     * print the characters literally and scroll the display instead of
     * redrawing over it. Each channel gets what it actually understands. */
    vga_home();

    serial_putc('\033');
    serial_putc('[');
    serial_putc('H');
}

/* Render an unsigned value into buf (which must hold at least 33 bytes) and
 * return it. Kept separate from output so padding can be applied afterwards. */
static char *format_uint(char *buf, uint32_t value, uint32_t base, bool upper)
{
    static const char *lower_digits = "0123456789abcdef";
    static const char *upper_digits = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;

    char tmp[32];
    int  i = 0;

    if (value == 0)
        tmp[i++] = '0';

    while (value > 0) {
        tmp[i++] = digits[value % base];
        value /= base;
    }

    int j = 0;
    while (i-- > 0)
        buf[j++] = tmp[i];
    buf[j] = '\0';

    return buf;
}

static char *format_int(char *buf, int32_t value)
{
    if (value < 0) {
        buf[0] = '-';
        /* Negating INT32_MIN overflows a signed 32-bit value, so widen first. */
        format_uint(buf + 1, (uint32_t)(-(int64_t)value), 10, false);
        return buf;
    }

    return format_uint(buf, (uint32_t)value, 10, false);
}

/* Emit a rendered field, applying width, alignment and fill.
 * Zero fill is ignored for left-aligned fields, matching printf. */
static void emit_field(const char *s, int width, bool left_align, bool zero_fill)
{
    int len = 0;
    while (s[len])
        len++;

    int padding = width - len;

    if (left_align) {
        kputs(s);
        while (padding-- > 0)
            kputc(' ');
        return;
    }

    /* A leading minus sign must stay in front of zero padding, or -42 with
     * %05d would come out as "000-42". */
    if (zero_fill && s[0] == '-') {
        kputc('-');
        s++;
        while (padding-- > 0)
            kputc('0');
        kputs(s);
        return;
    }

    while (padding-- > 0)
        kputc(zero_fill ? '0' : ' ');

    kputs(s);
}

/* Supports %[-][0][width](c|s|d|i|u|x|X|p|%). */
static void vkprintf(const char *fmt, va_list args)
{
    char buf[36];

    while (*fmt) {
        if (*fmt != '%') {
            kputc(*fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        bool left_align = false;
        bool zero_fill  = false;
        int  width      = 0;

        /* Flags. '-' and '0' may appear in either order. */
        for (;;) {
            if (*fmt == '-')      { left_align = true;  fmt++; }
            else if (*fmt == '0') { zero_fill  = true;  fmt++; }
            else break;
        }

        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'c':
            buf[0] = (char)va_arg(args, int);
            buf[1] = '\0';
            emit_field(buf, width, left_align, false);
            break;

        case 's': {
            const char *s = va_arg(args, const char *);
            emit_field(s ? s : "(null)", width, left_align, false);
            break;
        }

        case 'd':
        case 'i':
            emit_field(format_int(buf, va_arg(args, int32_t)),
                       width, left_align, zero_fill);
            break;

        case 'u':
            emit_field(format_uint(buf, va_arg(args, uint32_t), 10, false),
                       width, left_align, zero_fill);
            break;

        case 'x':
            emit_field(format_uint(buf, va_arg(args, uint32_t), 16, false),
                       width, left_align, zero_fill);
            break;

        case 'X':
            emit_field(format_uint(buf, va_arg(args, uint32_t), 16, true),
                       width, left_align, zero_fill);
            break;

        case 'p':
            kputs("0x");
            emit_field(format_uint(buf, (uint32_t)(uintptr_t)va_arg(args, void *),
                                   16, false), 8, false, true);
            break;

        case '%':
            kputc('%');
            break;

        case '\0':
            return;     /* trailing '%' at end of string */

        default:
            /* Unknown conversion: echo it rather than silently swallowing it,
             * so the mistake is visible in the output. */
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

    /* The console is shared state like any other. Without this, a task
     * preempted partway through a call has its output spliced by whatever
     * runs next -- "c15" arriving as "c" ... "15" with another task's text
     * wedged between. Harmless to the kernel, but it makes concurrent output
     * unreadable and is a poor advertisement in a system that spends its
     * time demonstrating synchronisation.
     *
     * Deferring preemption rather than taking a mutex is deliberate: a fault
     * handler prints, and a fault can occur inside a kprintf. A lock would
     * deadlock against its own holder there, whereas a deferral counter
     * simply nests. */
    preempt_disable();

    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);

    preempt_enable();
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
