#include "heap.h"
#include "console.h"

/* Every allocation is preceded by this header. The magic value turns silent
 * heap corruption into something detectable: if a caller writes past the end
 * of its block it will usually clobber the next header, and heap_check()
 * will find it. */
#define BLOCK_MAGIC 0xC0FFEE00

typedef struct block {
    uint32_t      magic;
    uint32_t      size;     /* usable bytes, not counting this header */
    struct block *next;
    struct block *prev;
    uint32_t      free;     /* 1 = available */
} block_t;

#define HEADER_SIZE   sizeof(block_t)

/* Splitting is only worthwhile if the remainder can hold a header plus a
 * useful amount of data. Below this we hand over the whole block. */
#define MIN_SPLIT     (HEADER_SIZE + 16)

/* All allocations are rounded up so returned pointers stay 8-byte aligned,
 * which every scalar type on i386 is happy with. */
#define ALIGNMENT     8
#define ALIGN_UP(n)   (((n) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

static block_t *head;
static uint32_t heap_start;
static uint32_t heap_size;

void heap_init(uint32_t start, uint32_t size)
{
    heap_start = start;
    heap_size  = size;

    /* One block covering everything. It gets carved up from here. */
    head = (block_t *)start;
    head->magic = BLOCK_MAGIC;
    head->size  = size - HEADER_SIZE;
    head->next  = 0;
    head->prev  = 0;
    head->free  = 1;

    kprintf("heap             : %u KB at %08x\n", size / 1024, start);
}

void *kmalloc(uint32_t size)
{
    if (size == 0)
        return 0;

    size = ALIGN_UP(size);

    /* First fit: take the first block big enough. Simpler than best-fit and
     * good enough here; best-fit reduces waste but costs a full list walk on
     * every allocation. */
    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* If the leftover is large enough to be useful, split the block so the
         * remainder stays available. Otherwise hand over the whole thing and
         * accept the small internal waste. */
        if (b->size >= size + MIN_SPLIT) {
            block_t *rest = (block_t *)((uint32_t)b + HEADER_SIZE + size);

            rest->magic = BLOCK_MAGIC;
            rest->size  = b->size - size - HEADER_SIZE;
            rest->free  = 1;
            rest->next  = b->next;
            rest->prev  = b;

            if (b->next)
                b->next->prev = rest;

            b->next = rest;
            b->size = size;
        }

        b->free = 0;
        return (void *)((uint32_t)b + HEADER_SIZE);
    }

    return 0;   /* exhausted */
}

/* Merge a block with its immediate successor if both are free. Called on the
 * block itself and on its predecessor, so a free between two free neighbours
 * collapses all three into one. */
static void coalesce(block_t *b)
{
    if (b && b->next && b->free && b->next->free) {
        b->size += HEADER_SIZE + b->next->size;
        b->next = b->next->next;

        if (b->next)
            b->next->prev = b;
    }
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    block_t *b = (block_t *)((uint32_t)ptr - HEADER_SIZE);

    if (b->magic != BLOCK_MAGIC) {
        /* Either not a heap pointer, or the header was overwritten by an
         * overflow in the preceding block. Either way, continuing would
         * corrupt the free list. */
        panic("kfree: bad block header at %p (magic %08x, expected %08x)",
              ptr, b->magic, BLOCK_MAGIC);
    }

    if (b->free)
        panic("kfree: double free at %p", ptr);

    b->free = 1;

    coalesce(b);            /* merge forward */
    coalesce(b->prev);      /* and backward */
}

void heap_get_stats(heap_stats_t *out)
{
    out->total_bytes  = heap_size;
    out->used_bytes   = 0;
    out->free_bytes   = 0;
    out->used_blocks  = 0;
    out->free_blocks  = 0;
    out->largest_free = 0;

    for (block_t *b = head; b; b = b->next) {
        if (b->free) {
            out->free_bytes += b->size;
            out->free_blocks++;
            if (b->size > out->largest_free)
                out->largest_free = b->size;
        } else {
            out->used_bytes += b->size;
            out->used_blocks++;
        }
    }
}

uint32_t heap_base(void)
{
    return heap_start;
}

uint32_t heap_size_bytes(void)
{
    return heap_size;
}

uint32_t heap_check(void)
{
    for (block_t *b = head; b; b = b->next) {
        if (b->magic != BLOCK_MAGIC)
            return (uint32_t)b;

        /* A block must lie inside the heap and must not claim more space
         * than physically remains. */
        uint32_t end = (uint32_t)b + HEADER_SIZE + b->size;
        if ((uint32_t)b < heap_start || end > heap_start + heap_size)
            return (uint32_t)b;

        if (b->next && (uint32_t)b->next < end)
            return (uint32_t)b;
    }

    return 0;
}
