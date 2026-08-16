#include "paging.h"
#include "console.h"

/* Page directory entry / page table entry flags. */
#define PAGE_PRESENT   0x001
#define PAGE_WRITE     0x002
#define PAGE_USER      0x004
#define PAGE_SIZE_4MB  0x080    /* PS bit: this directory entry maps 4 MB directly */

#define PAGE_SIZE_BYTES 4096
#define LARGE_PAGE_BYTES (4 * 1024 * 1024)
#define ENTRIES 1024

/* Both tables must be 4 KB aligned; the low 12 bits of the addresses stored in
 * CR3 and in directory entries are flag bits, not address bits. */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(4096)));

/* A 4 KB-granular table for the first 4 MB.
 *
 * The rest of memory is mapped with 4 MB pages, which is far simpler: one
 * directory entry per 4 MB and no second level at all. But the first 4 MB
 * needs finer granularity, because leaving a *single* page unmapped at address
 * zero is what makes a null dereference fault. A 4 MB page there would force a
 * choice between mapping the kernel (which lives at 1 MB) and trapping null. */
static uint32_t first_table[ENTRIES] __attribute__((aligned(4096)));

static bool     enabled;
static uint32_t mapped_bytes;

uint32_t paging_fault_address(void)
{
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void paging_init(uint32_t memory_bytes)
{
    /* Cap the mapping at what the directory can describe, and at what the
     * machine actually has. */
    if (memory_bytes > 0xC0000000u)
        memory_bytes = 0xC0000000u;

    /* --- first 4 MB, page by page --- */
    for (uint32_t i = 0; i < ENTRIES; i++) {
        if (i == 0) {
            /* Leave virtual 0x00000000-0x00000FFF unmapped. This is the entire
             * reason for the second level: a null pointer dereference now
             * raises a page fault with CR2 = 0, instead of quietly reading
             * whatever sits at physical address zero. */
            first_table[i] = 0;
            continue;
        }

        /* PAGE_USER makes this page reachable from ring 3.
         *
         * Setting it across the whole first 4 MB is a deliberate simplification
         * with a real cost: user code and kernel code share these pages, so
         * memory is NOT isolated between them — a ring 3 task could read kernel
         * data if it tried. Separating them properly would mean giving user
         * code its own linker section on its own pages.
         *
         * What is genuinely enforced here is instruction privilege: ring 3
         * cannot execute port I/O, cli/sti, or control register access, and the
         * CPU kills it for trying. That is what the user-mode demo shows, and
         * the limitation above is stated rather than glossed over. */
        first_table[i] = (i * PAGE_SIZE_BYTES)
                       | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    /* PAGE_USER is required on the directory entry as well as on the entries
     * in the table it points to.
     *
     * The CPU computes the effective permission as the AND of every level of
     * the walk, so a user-accessible page reached through a kernel-only
     * directory entry is still kernel-only. Setting the flag on the leaves and
     * forgetting the branch produces a page fault on the very first ring 3
     * instruction fetch, with error code 5: present, read, user mode — a
     * protection violation rather than a missing page, which is the clue that
     * the mapping exists but the permissions do not allow it. */
    page_directory[0] = ((uint32_t)first_table)
                      | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    mapped_bytes = LARGE_PAGE_BYTES - PAGE_SIZE_BYTES;

    /* --- everything above 4 MB, in 4 MB pages --- */
    uint32_t large_pages = memory_bytes / LARGE_PAGE_BYTES;

    for (uint32_t i = 1; i < ENTRIES; i++) {
        if (i < large_pages) {
            page_directory[i] = (i * LARGE_PAGE_BYTES)
                              | PAGE_PRESENT | PAGE_WRITE | PAGE_SIZE_4MB;
            mapped_bytes += LARGE_PAGE_BYTES;
        } else {
            /* Not present. Touching an address up here faults, which is what
             * the `fault page <addr>` demo relies on. */
            page_directory[i] = 0;
        }
    }

    /* Order matters. CR4.PSE must be set before any 4 MB entry is used, and
     * CR3 must point at a complete directory before paging is switched on:
     * the instruction immediately after the enable is fetched through the
     * MMU, so if the kernel is not mapped it faults, and the fault handler is
     * not mapped either, which triple-faults and resets the machine.
     *
     * Identity mapping is what makes this safe: eip means the same thing
     * before and after the switch. */
    __asm__ volatile (
        "movl %%cr4, %%eax      \n"
        "orl  $0x00000010, %%eax\n"     /* CR4.PSE — enable 4 MB pages */
        "movl %%eax, %%cr4      \n"

        "movl %0, %%eax         \n"
        "movl %%eax, %%cr3      \n"     /* point at the directory */

        "movl %%cr0, %%eax      \n"
        "orl  $0x80000000, %%eax\n"     /* CR0.PG — paging on */
        "movl %%eax, %%cr0      \n"
        :
        : "r"((uint32_t)page_directory)
        : "eax", "memory"
    );

    enabled = true;

    kprintf("paging           : enabled, %u MB identity mapped, page 0 unmapped\n",
            mapped_bytes / (1024 * 1024));
}

bool paging_enabled(void) { return enabled; }

uint32_t paging_mapped_bytes(void) { return mapped_bytes; }
