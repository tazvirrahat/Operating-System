#include "gdt.h"
#include "console.h"

/* One GDT entry. The layout is historical and awkward: the base and limit
 * fields are split across non-adjacent bytes because the format grew from
 * the 16-bit 80286 design. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* high 4 bits are flags, low 4 are limit bits 16-19 */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;         /* size of the table in bytes, minus one */
    uint32_t base;
} __attribute__((packed));

/* Task State Segment. In a modern kernel this exists almost solely so the CPU
 * knows which stack to switch to on a ring 3 -> ring 0 transition. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;          /* kernel stack pointer, the field we care about */
    uint32_t ss0;           /* kernel stack segment */
    uint32_t unused[23];
} __attribute__((packed));

#define GDT_ENTRIES 6

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_ptr;
static struct tss_entry tss;

static void gdt_set(int idx, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran)
{
    gdt[idx].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);

    gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[idx].granularity |= gran & 0xF0;

    gdt[idx].access = access;
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

void gdt_init(void)
{
    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint32_t)&gdt;

    /* Access byte:  P DPL S  E DC RW A
     *   P=1 present, DPL=ring, S=1 code/data,
     *   E=1 code, DC=direction/conforming, RW=readable/writable, A=accessed
     * Granularity:  G D 0 A  + limit bits 19-16
     *   G=1 limit counts 4 KB pages, D=1 32-bit operands
     *
     * Limit 0xFFFFF with G=1 gives 0xFFFFF * 4 KB = the full 4 GB. */
    gdt_set(0, 0, 0x00000, 0x00, 0x00);          /* null descriptor, required */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);          /* ring 0 code: present, exec, read */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);          /* ring 0 data: present, write */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);          /* ring 3 code (DPL=3) */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);          /* ring 3 data (DPL=3) */

    /* TSS descriptor. Access 0x89 = present, DPL 0, type 9 (32-bit TSS). */
    tss.ss0  = GDT_KERNEL_DATA;
    tss.esp0 = 0;                                /* filled in once tasks exist */
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    /* Load the table, then reload every segment register. CS cannot be set by
     * mov, so a far jump is used: it reloads CS as a side effect of jumping.
     * The jump target is the very next instruction. */
    __asm__ volatile (
        "lgdt %0                \n"
        "mov  $0x10, %%ax       \n"
        "mov  %%ax, %%ds        \n"
        "mov  %%ax, %%es        \n"
        "mov  %%ax, %%fs        \n"
        "mov  %%ax, %%gs        \n"
        "mov  %%ax, %%ss        \n"
        "ljmp $0x08, $1f        \n"
        "1:                     \n"
        :
        : "m"(gdt_ptr)
        : "eax", "memory"
    );

    /* Load the task register with the TSS selector. */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));

    kprintf("gdt              : 6 descriptors, flat 4GB, ring 0 + ring 3\n");
}
