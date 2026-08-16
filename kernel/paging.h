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

void paging_init(uint32_t memory_bytes);

/* True once the MMU is on. */
bool paging_enabled(void);

/* Total bytes currently mapped. */
uint32_t paging_mapped_bytes(void);

/* Read CR2, which the CPU fills with the faulting linear address. */
uint32_t paging_fault_address(void);

#endif /* PAGING_H */
