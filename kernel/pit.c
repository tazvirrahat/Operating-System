#include "pit.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "console.h"

#define PIT_CH0_DATA 0x40
#define PIT_COMMAND  0x43

/* The PIT is driven by a fixed 1.193182 MHz oscillator, a number inherited
 * from the original IBM PC where it was derived from the NTSC colour burst
 * frequency. The divisor sets how many input ticks pass per output pulse. */
#define PIT_BASE_FREQUENCY 1193182

static volatile uint32_t ticks;
static uint32_t frequency;
static tick_callback_t on_tick;

static void timer_isr(registers_t *regs)
{
    (void)regs;

    ticks++;

    if (on_tick)
        on_tick();
}

void pit_init(uint32_t hz)
{
    frequency = hz;
    ticks = 0;

    uint32_t divisor = PIT_BASE_FREQUENCY / hz;

    /* Command 0x36: channel 0, access mode lo/hi byte, mode 3 (square wave),
     * binary (not BCD) counting. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    isr_register(IRQ_BASE + IRQ_TIMER, timer_isr);
    pic_unmask(IRQ_TIMER);

    kprintf("pit              : %u Hz (divisor %u), irq 0 unmasked\n", hz, divisor);
}

uint32_t pit_ticks(void)
{
    return ticks;
}

uint32_t pit_hz(void)
{
    return frequency;
}

void pit_on_tick(tick_callback_t cb)
{
    on_tick = cb;
}
