/* paging.h — virtual memory.
 *
 * Enables the MMU with an identity mapping: virtual address X maps to physical
 * address X everywhere, so no existing pointer changes meaning. What paging
 * buys us here is not relocation but *protection* — an address with no mapping
 * now raises a page fault instead of silently reading whatever happens to be
 * on the bus.
 *
 * Page 0 is deliberately left unmapped so that dereferencing a null pointer
 * faults, exactly as it does on a real operating system. Without paging every
 * linear address is valid and a null dereference quietly returns garbage.
 */
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>

/* `fb_addr` and `fb_size` describe a framebuffer to map in addition to RAM.
 * Video memory typically sits far above installed memory — a mapping covering
 * only reported RAM would leave it unmapped, and the first pixel written would
 * page fault. Pass 0 if there is no framebuffer. */
void paging_init(uint32_t memory_bytes, uint32_t fb_addr, uint32_t fb_size);

/* Map a physical region after paging is already running.
 *
 * Device memory found by PCI enumeration cannot be mapped at startup, because
 * its address is not known until the bus has been scanned. Adding directory
 * entries afterwards is safe as long as the TLB is invalidated, which this
 * does — a stale entry would otherwise keep reporting the region as absent.
 */
void paging_map_region(uint32_t addr, uint32_t size);

/* True once the MMU is on. */
bool paging_enabled(void);

/* Total bytes currently mapped. */
uint32_t paging_mapped_bytes(void);

/* Read CR2, which the CPU fills with the faulting linear address. */
uint32_t paging_fault_address(void);

/* Page-table flag bits, matching the hardware. Exposed so a visualisation
 * can name the same bits the walk used rather than duplicating the masks. */
#define PTE_PRESENT  0x001
#define PTE_WRITE    0x002
#define PTE_USER     0x004
#define PTE_LARGE    0x080

#define PAGING_ENTRIES     1024
#define PAGING_PAGE_SIZE   4096
#define PAGING_LARGE_SIZE  (4u * 1024u * 1024u)

/* One step of the hardware walk, reported rather than performed: the MMU
 * already did this for every fetch. `phys` is 0 when the address is not
 * present — including page 0, which is left unmapped on purpose. */
typedef struct {
    uint32_t virt;
    uint32_t dir_index;
    uint32_t tab_index;
    uint32_t pde;
    uint32_t pte;           /* 0 when the directory entry is a 4 MB page */
    uint32_t phys;
    bool     present;
    bool     large;
    bool     user;
    bool     writable;
} page_walk_t;

void     paging_walk(uint32_t virt, page_walk_t *out);
uint32_t paging_pde(uint32_t index);

#endif /* PAGING_H */
