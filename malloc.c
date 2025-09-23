#include <assert.h>
#include <unistd.h> /* getpagesize */
#include <sys/mman.h> /* mmap */
#include <string.h> /* memset */

#include "malloc.h"
#include "internal.h"

/* A blank box for diagrams :>
 * ┌────────────────────────┐
 * │                        │
 * ├────────────────────────┤
 * └────────────────────────┘
 */

/* The bookkeeping consists of two layers.
 *
 * A span is a group of contiguous pages requested from the OS with mmap(2).
 * Its header holds the original size of the span, necessary because munmap
 * needs both the address and length; the number of free bytes, to determine if
 * a malloc() request can be served by this span; and a pointer to the next
 * span being tracked. Each span has its own free list of blocks.
 *
 * A block is a logical chunk of memory within a span, allocated to serve a
 * malloc() call. It also holds size information and a pointer to the next free
 * block in the span.
 *
 * ┌────────────────────────┐
 * │struct span             │ ───┐
 * ├────────────────────────┤    │
 * │struct block (in use)   │    │
 * ├────────────────────────┤    │
 * │01010101...             │    │
 * │10101010...             │    │
 * │01010101...             │    │
 * │[padding to 16 bytes]   │    │
 * ├────────────────────────┤    │
 * │struct block (free)     │ <──┘
 * ├────────────────────────┤ ───┐
 * │                        │    │
 * │                        │    │
 * ├────────────────────────┤    │
 * │struct block (free)     │ <──┘
 * ├────────────────────────┤
 * │                        │
 * │                        │
 * └────────────────────────┘
 */

void assert_aligned(usz x, usz a) {
    assert(x % a == 0);
}

/* Align a pointer to the next multiple of 16 address.
 */
void *align_ptr(void *p) {
    uptr x = (uptr)p;
    uptr y = (x + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
    return (void *)y;
}

/* Get a pointer to the first block header after a span header, considering
 * padding.
 */
struct block *first_block(struct span *sp) {
    return (struct block *)((char *)sp + SPAN_HDR_PADSZ);
}

/* The initial state, when no pages have been requested from the OS, is that
 * the global base pointer is NULL. When the first request comes, the initial
 * span is tracked by this pointer.
 */
struct span *base = 0;

/* The page size is requested and stored here upon the first call to malloc().
 */
int pagesize = 0;

/* mmap obtains memory in multiples of the page size, padding up the requested
 * size if necessary. Therefore it's in our best interest to round up the
 * request to a page boundary as well, to get that extra memory to ourselves.
 *
 * To minimize system calls for small allocations, a minimum allocation size of
 * MINIMUM_ALLOCATION is requested.
 */
usz usz_max(usz a, usz b) {
    return a > b ? a : b;
}

struct span *alloc_span(usz gross) {
    usz spsz = usz_max(gross, MINIMUM_ALLOCATION);
    spsz = ALIGN_UP(spsz, pagesize);

    /* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
     */
    struct span *sp = mmap(0, spsz, PROT_WRITE | PROT_READ,
        MAP_ANON | MAP_PRIVATE, -1, 0);

    if (sp == MAP_FAILED)
        return 0;

    sp->size = spsz;
    sp->next = base;    /* Prepend the span to the list. */
    base = sp;

    /* Place one initial all-spanning free block immediately after the span
     * header.
     */
    sp->free_list = first_block(sp);
    sp->free_list->size = spsz - (usz)SPAN_HDR_PADSZ;
    sp->free_list->prev = 0;
    sp->free_list->next = 0;
    sp->free_list->owner = sp;
    sp->free_list->free = 1;
    sp->free_list->magic = MAGIC_BABY;

    return sp;
}

/* Remove sp from the list of spans.
 */
void sever_span(struct span *sp) {
    if (sp == base) {
        base = sp->next;
        sp->next = 0;
    } else {
        /* sp is not first. Find the previous span.
         */
        struct span *p = base;
        while (p && p->next && p->next != sp)
            p = p->next;
        p->next = sp->next;
        sp->next = 0;
    }
}

/* Return an entire span to the OS.
 * Eventually this could assert on a count of free blocks.
 * XXX return the value from munmap?
 */
void free_span(struct span *sp) {
    sever_span(sp);
    munmap(sp, sp->size);
}

/* Take block bp off of its span's free list.
 */
void sever_block(struct block *bp) {
    struct span *sp = bp->owner;

    if (bp->next) assert(bp->next->prev == bp);
    if (bp->prev) assert(bp->prev->next == bp);
    else          assert(sp->free_list == bp);

    if (!bp->prev) {
        /* bp is first in the free list. Point the span to whatever is next,
         * and make that the start of the list.
         */
        sp->free_list = bp->next;
        if (sp->free_list)
            sp->free_list->prev = 0;
    } else {
        /* Point the previous block to bp's next block, and vice versa (if
         * there is a next block).
         */
        bp->prev->next = bp->next;
        if (bp->next)
            bp->next->prev = bp->prev;
    }
}

/* Reduce the size of bp and create a new block at the end of its free space,
 * with size gross.
 */
struct block *split_block(usz gross, struct block *bp) {
    assert(bp && bp->size > gross);
    struct span *sp = bp->owner;

    /* Compute new block position.
     */
    byte *nb = (byte *)bp + bp->size - gross;
    assert((uptr)nb % ALIGNMENT == 0); /* nb is aligned */
    /* nb landed within the span */
    assert((uptr)sp < (uptr)nb && (uptr)nb < ((uptr)sp + sp->size));

    bp->size -= gross;  /* Make free block smaller and leave it in the list. */

    /* gross is already aligned, so it is safe to place a new header there.
     */
    bp = (struct block *)nb;
    bp->size = gross;
    bp->owner = sp;
    bp->free = 0;               /* Occupied. */
    bp->prev = bp->next = 0;    /* Not strictly necessary. */
    bp->magic = MAGIC_SPENT;    /* Take the poison. */
    return bp;
}

/* Use the given block to serve a malloc() request. If the block is big enough
 * to split, the request is served with a new block placed at the end of the
 * free block. The free block is reduced and left in the free list.
 */
struct block *alloc_block(usz gross, struct block *bp) {
    assert(bp && bp->free);

    /* bp points to a currently free block. Its size is bigger than or equal to
     * gross. If the remaining space after splitting is too small, take the
     * fragmentation and assign the entire block. Otherwise, split.
     */
    if (bp->size - gross < MINIMUM_BLKSZ) {
        sever_block(bp);
        /* No need to update bp's size. Its entire size is already correct.
         */
        bp->free = 0;               /* Occupied. */
        bp->prev = bp->next = 0;    /* Not strictly necessary. */
        bp->magic = MAGIC_SPENT;    /* Take the poison. */
    } else {
        /* split_block takes care of fully initializing the new block.
         */
        bp = split_block(gross, bp);
    }

    return bp;
}

/* Traverse the free list of each span to find a free block big enough to serve
 * a request. The given size is the gross size--enough to hold the header and
 * the memory.
 */
struct block *find_block(usz gross) {
    struct span *sp = base;
    while (sp) {
        struct block *bp = sp->free_list;
        while (bp) {
            if (bp->size >= gross)
                return bp;
            bp = bp->next;
        }
        sp = sp->next;
    }
    return 0;
}

/* Calculate the gross size needed to serve a user request for `size` bytes.
 * The gross size includes the block header and its padding, the requested
 * memory, and padding after the memory to fill to the next ALIGNMENT boundary
 * (so the next block header will also be aligned).
 */
usz gross_size(usz size) {
    return BLOCK_HDR_PADSZ + ALIGN_UP(size, ALIGNMENT);
}

/* Serve a request for memory for the caller. Search for an already mmap'd span
 * with enough available space for the new block: its header, and the number of
 * bytes requested by the user. If one does not exist, a new span is mmap'd and
 * linked to the span list, and used to serve the request.
 */
void *m_malloc(usz size) {
    if (size == 0)
        return 0;

    /* Determine the page size on first call.
     */
    if (pagesize == 0)
        pagesize = getpagesize();

    /* Calculate how many bytes are needed to hold the requested memory along
     * with padding and metadata.
     */
    usz gross = gross_size(size);

    /* Try to find a block with enough space to serve the request.
     */
    struct block *bp = find_block(gross);

    /* If no existing span has enough space to serve the request, or if there
     * is no existing span because this is the first call, a new span needs to
     * be requested from the OS.
     */
    if (bp == 0) {
        struct span *sp = alloc_span(gross);
        if (sp == 0)     /* mmap(2) failed, not my fault */
            return 0;

        /* The fresh span has a single free block the size of the entire span.
         */
        bp = sp->free_list;
    }

    /* Allocate the block at bp to the caller. Split the free space if
     * possible, sever the block from the free list, and update block and span
     * metadata.
     */
    bp = alloc_block(gross, bp);

    /* The caller's memory comes after the block header, which is padded to
     * ALIGNMENT bytes to ensure the memory itself is aligned. The memory is
     * extended to a multiple of ALIGNMENT too, to ensure any subsequent
     * block header is automatically aligned.
     */
    return bp + 1;
}

/* Find the struct block * from a void * given by malloc.
 */
struct block *block_from_payload(void *p) {
    return (struct block *)((char *)p - BLOCK_HDR_PADSZ);
}

/* Give back a block of memory to its span.
 */
void m_free(void *p) {
    if (!p)
        return;

    struct block *bp = block_from_payload(p);
    assert(!bp->free);

    struct span *sp = bp->owner;

    bp->free = 1;
    bp->magic = MAGIC_BABY;
    bp->prev = 0;
    bp->next = sp->free_list;
    sp->free_list = bp;
    if (bp->next)
        bp->next->prev = bp;

    /* Poison the payload for visibility.
     */
    memset(p, 0xae, bp->size);
}
