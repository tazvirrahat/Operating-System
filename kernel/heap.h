/* heap.h — dynamic memory allocator.
 *
 * A first-fit free-list allocator over a fixed region of physical memory.
 * Blocks are split when oversized and merged with their neighbours when freed,
 * which is what keeps the heap from fragmenting into uselessness.
 *
 * There is no virtual memory underneath this: addresses handed out are real
 * physical addresses.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t largest_free;
} heap_stats_t;

void  heap_init(uint32_t start, uint32_t size);

/* Returns NULL when no block is large enough. Every caller must check. */
void *kmalloc(uint32_t size);

void  kfree(void *ptr);

void  heap_get_stats(heap_stats_t *out);

/* Base address of the heap region, for reporting. */
uint32_t heap_base(void);
uint32_t heap_size_bytes(void);

/* Walks the block list checking header magic and size consistency.
 * Returns 0 if intact, or the address of the first corrupt block. */
uint32_t heap_check(void);

#endif /* HEAP_H */
