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

void assert_ptr_aligned(void *p, usz a) {
    /* XXX how do uintptr_t and size_t relate?
     */
    assert((uptr)p % a == 0);
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
    if (sp->next)
        sp->next->prev = sp;
    base = sp;

    /* Place one initial all-spanning free block immediately after the span
     * header.
     */
    sp->free_list = first_block(sp);
    sp->free_list->prev = 0;
    sp->free_list->next = 0;
    sp->free_list->owner = sp;
    sp->free_list->magic = MAGIC_BABY;
    usz size = spsz - (usz)SPAN_HDR_PADSZ;
    blksetsize(sp->free_list, size);
    blksetfree(sp->free_list);
    *blkfoot(sp->free_list) = size;

    return sp;
}

/* Remove sp from the list of spans.
 */
void sever_span(struct span *sp) {
    if (sp == base) {
        base = sp->next;
        sp->next = 0;
        if (base)
            base->prev = 0;
    } else {
        assert(sp->prev);
        sp->prev->next = sp->next;
        if (sp->next)
            sp->next->prev = sp->prev;
        sp->prev = 0;
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
struct block *blksplit(struct block *bp, usz gross) {
    assert(bp && blksize(bp) > gross);
    struct span *sp = bp->owner;

    /* Compute new block position.
     */
    byte *nb = (byte *)bp + blksize(bp) - gross;
    assert((uptr)nb % ALIGNMENT == 0); /* nb is aligned */
    /* nb landed within the span */
    assert((uptr)sp < (uptr)nb && (uptr)nb < ((uptr)sp + sp->size));

    /* Make free block smaller and leave it in the list. */
    usz bsz = blksize(bp) - gross;
    blksetsize(bp, bsz);
    *blkfoot(bp) = bsz;

    /* gross is already aligned, so it is safe to place a new header there.
     */
    bp = (struct block *)nb;
    bp->owner = sp;
    bp->prev = bp->next = 0;    /* Not strictly necessary. */
    bp->magic = MAGIC_SPENT;    /* Take the poison. */
    blksetsize(bp, gross);
    blksetused(bp);
    blksetprevfree(bp);         /* Not strictly necessary? setsize clears
                                   IN_USE bit */
    return bp;
}

/* Use the given block to serve a malloc() request. If the block is big enough
 * to split, the request is served with a new block placed at the end of the
 * free block. The free block is reduced and left in the free list.
 */
struct block *blkalloc(usz gross, struct block *bp) {
    assert(bp && blkisfree(bp));

    /* bp points to a currently free block. Its size is bigger than or equal to
     * gross. If the remaining space after splitting is too small, take the
     * fragmentation and assign the entire block. Otherwise, split.
     */
    if (blksize(bp) - gross < MINIMUM_BLKSZ) {
        sever_block(bp);
        /* No need to update bp's size. Its entire size is already correct.
         */
        blksetused(bp);
        bp->prev = bp->next = 0;    /* Not strictly necessary. */
        bp->magic = MAGIC_SPENT;    /* Take the poison. */
    } else {
        /* blksplit takes care of fully initializing the new block.
         */
        bp = blksplit(bp, gross);
    }

    /* Let the next block know its prev neighbor is in use.
     */
    struct block *bn = blknextadj(bp);
    if (bn)
        blksetprevused(bn);

    return bp;
}

/* Return a block to its span's free list.
 */
void blkfree(struct block *bp) {
    struct span *sp = bp->owner;

    blksetfree(bp);
    *blkfoot(bp) = blksize(bp);
    bp->magic = MAGIC_BABY;
    bp->prev = 0;
    bp->next = sp->free_list;
    sp->free_list = bp;
    if (bp->next)
        bp->next->prev = bp;

    /* Tell next physical block that prev is free.
     */
    struct block *bn = blknextadj(bp);
    if (bn)
        blksetprevfree(bn);
}

/* Traverse the free list of each span to find a free block big enough to serve
 * a request. The given size is the gross size--enough to hold the header and
 * the memory.
 */
struct block *blkfind(usz gross) {
    struct span *sp = base;
    while (sp) {
        struct block *bp = sp->free_list;
        while (bp) {
            if (blksize(bp) >= gross)
                return bp;
            bp = bp->next;
        }
        sp = sp->next;
    }
    return 0;
}

/* Compute a pointer to the (free) block physically before bp using its footer
 * (hence the need for it to be free). If bp is the first block in the span,
 * return 0.
 */
struct block *blkprevadj(struct block *bp) {
    assert(blkisprevfree(bp));

    struct span *sp = bp->owner;
    usz *ft = blkprevfoot(bp);

    /* ft landed inside the span header. bp is the first block in the span.
     */
    if ((uptr)ft < (uptr)sp + SPAN_HDR_PADSZ)
        return 0;

    struct block *bq = (struct block *)((uptr)bp - *ft);
    assert_ptr_aligned(bq, ALIGNMENT);

    return bq;
}

/* Compute a pointer to the block physically next to bp. If that block is in
 * use, it will not be bp->next. If bp is the last block in its span, return
 * 0.
 */
struct block *blknextadj(struct block *bp) {
    struct span *sp = bp->owner;
    uptr next = (uptr)bp + blksize(bp);

    /* Header and payload must be aligned for that to work.
     */
    assert_aligned(next, ALIGNMENT);

    /* The final block is given the last few bytes in the span.
     */
    if (next >= (uptr)sp + sp->size)
        return 0;
    return (struct block *)next;
}

/* Calculate the gross size needed to serve a user request for `size` bytes.
 * The gross size includes the block header and its padding, the requested
 * memory, and padding after the memory to fill to the next ALIGNMENT boundary
 * (so the next block header will also be aligned).
 */
usz gross_size(usz size) {
    return BLOCK_HDR_PADSZ + ALIGN_UP(size, ALIGNMENT);
}

/* Find the struct block * from a void * given by malloc.
 */
struct block *block_from_payload(void *p) {
    return (struct block *)((char *)p - BLOCK_HDR_PADSZ);
}

/* Get a pointer to the aligned memory owned by a block header.
 */
void *payload_from_block(struct block *bp) {
    return (char *)bp + BLOCK_HDR_PADSZ;
}

/* Join blocks by extending bp to cover bq and removing bq from the free list.
 * Bq must be the next adjacent block after bp, and be free. Importantly, bq
 * is no longer a valid block pointer after calling coalesce(), since it points
 * into the middle of a block.
 */
void coalesce(struct block *bp, struct block *bq) {
    assert(bp && bq);
    assert(blknextadj(bp) == bq);
    assert(blkisfree(bp) && blkisfree(bq));

    /* Remove bq from the free list to ensure it's no longer allocated.
     */
    sever_block(bq);

    usz bsz = blksize(bp) + blksize(bq);
    blksetsize(bp, bsz);
    *blkfoot(bp) = bsz;
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
    struct block *bp = blkfind(gross);

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
    bp = blkalloc(gross, bp);

    /* The caller's memory comes after the block header, which is padded to
     * ALIGNMENT bytes to ensure the memory itself is aligned. The memory is
     * extended to a multiple of ALIGNMENT too, to ensure any subsequent
     * block header is automatically aligned.
     */
    return payload_from_block(bp);
}

/* Give back a block of memory to its span.
 */
void m_free(void *p) {
    if (!p)
        return;

    struct block *bp = block_from_payload(p);
    assert(!blkisfree(bp));
    blkfree(bp);

    /* Coalesce in both directions.
     */
    struct block *bq = blknextadj(bp);
    if (bq && blkisfree(bq))
        coalesce(bp, bq);

    if (blkisprevfree(bp)) {
        if ((bq = blkprevadj(bp))) {
            coalesce(bq, bp);
            bp = bq; /* Make bp point to a valid free block again. */
            p = payload_from_block(bp); /* Same thing for p. */
        }
    }

    /* Poison the block for visibility; skip the footer.
     */
    memset(p, POISON_BYTE, blksize(bp) - BLOCK_HDR_PADSZ - sizeof(usz));
}
