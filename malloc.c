#include <assert.h>
#include <unistd.h> /* sysconf */
#include <sys/mman.h> /* mmap */
#include <string.h> /* memset, memcpy */

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
 * Spans are organized in a doubly linked list.
 *
 * A block is a logical chunk of memory within a span, allocated to serve a
 * malloc() call. Free blocks are organized in a doubly linked list. The span
 * that owns them has a pointer to the head of that list. The least significant
 * bits of the block size indicate whether the block is in use, and whether the
 * previous physically adjacent block is in use.
 *
 * ┌────────────────────────┐
 * │struct span             │ ───┐
 * ├────────────────────────┤    │ sp->free_list
 * │struct block (free)     │ <──┘
 * ├────────────────────────┤ ───┐ bp->next
 * │                        │    │
 * │                        │    │
 * │[footer: block size]    │    │
 * ├────────────────────────┤    │
 * │struct block (in use)   │    │
 * ├────────────────────────┤    │
 * │01010101...             │    │
 * │10101010...             │    │
 * │01010101...             │    │
 * │[padding to 16 bytes]   │    │
 * ├────────────────────────┤    │
 * │struct block (free)     │ <──┘
 * ├────────────────────────┤
 * │                        │
 * │[footer: block size]    │
 * └────────────────────────┘
 */

/* Align a pointer to the next multiple of 16 address.
void *align_ptr(void *p) {
    uptr x = (uptr)p;
    uptr y = (x + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
    return (void *)y;
}
*/

/* The initial state, when no pages have been requested from the OS, is that
 * the global base pointer is NULL. When the first request comes, the initial
 * span is tracked by this pointer.
 */
struct span *base = 0;

/* The page size is requested and stored here upon the first call to malloc().
 */
int pagesize = 0;

/* The number of spans in the list.
 */
int span_count = 0;

/* Get a pointer to the first block header after a span header, considering
 * padding.
 */
struct block *spfirstblk(struct span *sp) {
    return (struct block *)((char *)sp + SPAN_HDR_PADSZ);
}

/* Request enough pages with mmap(2) to fit an allocation of gross bytes as
 * well as a span header.
 */
struct span *spalloc(usz gross) {
    /* mmap obtains memory in multiples of the page size, padding up the
     * requested size if necessary. Therefore it's in our best interest to
     * round up the request to a page boundary as well, to get that extra
     * memory to ourselves.
     *
     * To minimize system calls for small allocations, a minimum allocation
     * size of MIN_MMAPSZ is requested.
     */
    usz spsz = usz_max(gross + (usz)SPAN_HDR_PADSZ, MIN_MMAPSZ);
    spsz = ALIGN_UP(spsz, pagesize);

    /* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
     */
    struct span *sp = mmap(0, spsz, PROT_WRITE | PROT_READ,
        MAP_ANON | MAP_PRIVATE, -1, 0);

    if (sp == MAP_FAILED)
        return 0;
    span_count++;

    sp->size = spsz;
    sp->blkcount = 0;
    sp->next = base;    /* Prepend the span to the list. */
    if (sp->next)
        sp->next->prev = sp;
    base = sp;

    /* Place one all-spanning free block immediately after the span header. */
    usz size = spsz - (usz)SPAN_HDR_PADSZ;
    sp->free_list = blkinitfree(spfirstblk(sp), sp, size);
    return sp;
}

/* Remove sp from the list of spans.
 */
void spsever(struct span *sp) {
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
 * XXX return the value from munmap?
 */
void spfree(struct span *sp) {
    span_count--;
    spsever(sp);
    munmap(sp, sp->size);
}

int ptr_in_span(void *p, struct span *sp) {
    uptr usp = (uptr)sp;
    uptr up = (uptr)p;
    return usp <= up && up <= usp + sp->size;
}

/* Take block bp off of its span's free list.
 */
void blksever(struct block *bp) {
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

    /* Compute new block position. */
    byte *nb = (byte *)bp + blksize(bp) - gross;
    assert_ptr_aligned(nb, ALIGNMENT);
    assert(ptr_in_span(nb, sp)); /* nb landed within the span */

    /* Make free block smaller and leave it in the list. */
    usz bsz = blksize(bp) - gross;
    blksetsize(bp, bsz);
    *blkfoot(bp) = bsz;

    /* gross is already aligned, so it is safe to place a new header there. */
    bp = blkinitused(nb, sp, gross);
    blksetprevfree(bp);
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
    if (blksize(bp) - gross < MIN_BLKSZ) {
        blksever(bp);
        blkinitused(bp, bp->owner, blksize(bp));
    } else {
        /* blksplit takes care of fully initializing the new block. */
        bp = blksplit(bp, gross);
    }

    bp->owner->blkcount++;

    /* Let the next block know its prev neighbor is in use. */
    struct block *bq = blknextadj(bp);
    if (bq)
        blksetprevused(bq);

    return bp;
}

/* Return a block to its span's free list.
 */
void blkfree(struct block *bp) {
    struct span *sp = bp->owner;
    assert(sp->blkcount > 0);
    sp->blkcount--;
    blkinitfree(bp, sp, blksize(bp));
    blkprepend(bp);

    struct block *bq = blknextadj(bp);
    if (bq)
        blksetprevfree(bq);
}

/* Initialize a block header at location p with the given size and owner.
 */
struct block *blkinit(void *p, struct span *sp, usz size) {
    assert(ptr_in_span(p, sp));
    assert_ptr_aligned(p, ALIGNMENT);
    assert(ptr_in_span((byte *)p + size, sp));

    struct block *bp = (struct block *)p;
    blksetsize(bp, size);
    bp->owner = sp;
    bp->next = bp->prev = 0;
    return bp;
}

/* Initialize a header at location p for a free block with the given size and
 * owner.
 */
struct block *blkinitfree(void *p, struct span *sp, usz size) {
    struct block *bp = blkinit(p, sp, size);
    bp->magic = MAGIC_BABY;
    blksetfree(bp);
    *blkfoot(bp) = size;
    return bp;
}

/* Initialize a header for an allocated block at location p, with the given
 * size and owner.
 */
struct block *blkinitused(void *p, struct span *sp, usz size) {
    struct block *bp = blkinit(p, sp, size);
    blksetused(bp);
    bp->magic = MAGIC_SPENT;
    return bp;
}

/* Prepend free block bp to its owner span's free list.
 */
void blkprepend(struct block *bp) {
    assert(bp && blkisfree(bp));
    struct span *sp = bp->owner;
    bp->next = sp->free_list;
    sp->free_list = bp;
    if (bp->next)
        bp->next->prev = bp;
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

    /* ft landed inside the span header. bp is the first block in the span. */
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

    /* Header and payload must be aligned for that to work. */
    assert_aligned(next, ALIGNMENT);

    /* The final block is given the last few bytes in the span. */
    if (next >= (uptr)sp + sp->size)
        return 0;
    return (struct block *)next;
}

/* Join blocks by extending bp to cover bq and removing bq from the free list.
 * Bq must be the next adjacent block after bp, and be free. Importantly, bq
 * is no longer a valid block pointer after calling coalesce(), since it points
 * into the middle of a block.
 */
void blkcoalesce(struct block *bp, struct block *bq) {
    assert(bp && bq);
    assert(blknextadj(bp) == bq);
    assert(blkisfree(bp) && blkisfree(bq));

    /* Remove bq from the free list to ensure it's no longer allocated. */
    blksever(bq);

    usz bsz = blksize(bp) + blksize(bq);
    blksetsize(bp, bsz);
    *blkfoot(bp) = bsz;
}

/* Try to coalesce a free block in both directions.
 */
struct block *coalesce(struct block *bp) {
    assert(bp && blkisfree(bp));

    struct block *bq = blknextadj(bp);
    if (bq && blkisfree(bq))
        blkcoalesce(bp, bq);

    if (blkisprevfree(bp)) {
        if ((bq = blkprevadj(bp))) {
            blkcoalesce(bq, bp); /* Intentional: extend bq over bp */
            bp = bq;
        }
    }

    return bp;
}

/* Serve a request for memory for the caller. Search for an already mmap'd span
 * with enough available space for the new block: its header, and the number of
 * bytes requested by the user. If one does not exist, a new span is mmap'd and
 * linked to the span list, and used to serve the request.
 */
void *m_malloc(usz size) {
    if (size == 0)
        return 0;

    /* Determine the page size on first call. */
    if (pagesize == 0)
        pagesize = sysconf(_SC_PAGESIZE);

    /* Calculate how many bytes are needed to hold the requested memory along
     * with padding and metadata.
     */
    usz gross = blksizerequest(size);
    assert(gross >= MIN_BLKSZ);

    /* Try to find a block with enough space to serve the request. */
    struct block *bp = blkfind(gross);

    /* If no existing span has enough space to serve the request, or if there
     * is no existing span because this is the first call, a new span needs to
     * be requested from the OS.
     */
    if (bp == 0) {
        struct span *sp = spalloc(gross);
        if (sp == 0)     /* mmap(2) failed, not my fault */
            return 0;

        /* The fresh span has a single free block the size of the entire span. */
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
    return blkpayload(bp);
}

/* Give back a block of memory to its span.
 */
void m_free(void *p) {
    if (!p)
        return;

    struct block *bp = plblk(p);
    assert(!blkisfree(bp));
    blkfree(bp);

    struct span *sp = bp->owner;
    if (sp->blkcount == 0 && span_count > SPAN_CACHE) {
        spfree(sp);
        return;
    }

    /* Coalesce in both directions. */
    bp = coalesce(bp);
    p = blkpayload(bp);

    /* Poison the block for visibility; skip the footer. */
    memset(p, POISON_BYTE, plsize(bp) - sizeof(usz));
}

/* Allocate enough contiguous space for n elements of size s bytes each. The
 * allocated memory is zeroed out.
 */
void *m_calloc(usz n, usz s) {
    s *= n;
    void *p = m_malloc(s);
    if (!p)
        return 0;
    struct block *bp = plblk(p);
    memset(p, 0, plsize(bp));
    return p;
}

/* Try to change the size of allocation p to size, and return p. If size is
 * larger than the current allocation and there is enough contiguous space to
 * extend the current allocation, the allocation is extended in place.
 * Otherwise, realloc() creates a new allocation, copies all the bytes from the
 * original allocation to the new one, and frees the old allocation. If p is
 * NULL, realloc() just returns a pointer to a new allocation for size bytes,
 * as if malloc(size) had been called. If p is not NULL and size is zero, the
 * allocation is truncated in place to a minimum-sized block.
 *
 * The returned allocation is not zeroed out.
 */
void *m_realloc(void *p, usz size) {
    /* Scenarios:
     * 1. There is no allocation (p == 0).
     * 2. The payload is reduced to minimum payload size (p && size == 0).
     * 3. The payload is reduced.
     * 4. The payload is extended and there is adjacent space in the block.
     * 5. The payload is extended but needs to be moved.
     */
    if (!p)
        return m_malloc(size);

    struct block *bp = plblk(p);
    usz gross = blksizerequest(size);

    if (gross == blksize(bp))
        return p;

    if (!size || gross < blksize(bp))
        return realloc_truncate(bp, size);

    return realloc_extend(bp, size);
}

void *realloc_truncate(struct block *bp, usz size) {
    assert(bp && !blkisfree(bp));

    void *p = blkpayload(bp);
    usz gross = blksizerequest(size);
    assert(MIN_BLKSZ <= gross && gross <= blksize(bp));

    /* Split if the resulting block and the resized block are big enough. */
    if (blksize(bp) - gross < MIN_BLKSZ || gross < MIN_BLKSZ)
        return blkpayload(bp);

    /* Truncate bp and place a new block in the free space. */
    usz nsz = blksize(bp) - gross;
    blksetsize(bp, gross);

    byte *nb = (byte *)bp + gross;
    assert_ptr_aligned(nb, ALIGNMENT);
    bp = blkinitfree(nb, bp->owner, nsz);
    blkprepend(bp);
    blksetprevused(bp);     /* The reduced block is still in use */

    /* Tell the next adjacent block about the new free block before it. */
    struct block *bq = blknextadj(bp);
    if (bq) {
        blksetprevfree(bq);
        bp = coalesce(bp);
    }

    /* p still points to the original payload, now truncated. */
    return p;
}

void *realloc_extend(struct block *bp, usz size) {
    assert(bp && !blkisfree(bp));

    usz gross = blksizerequest(size);
    assert(blksize(bp) < gross);

    void *p = blkpayload(bp);
    struct block *bq = blknextadj(bp);

    usz diff = bq && blkisfree(bq) ? gross - blksize(bp) : 0;
    if (bq && blkisfree(bq) && blksize(bq) >= diff) {
        /* Extend bp over bq, splitting if there's enough space left.
         *
         * [    bp     ][   bq   ]
         *  ------ gross ------## <- leftover
         */
        usz leftover = blksize(bp) + blksize(bq) - gross;
        assert_aligned(leftover, ALIGNMENT);

        if (leftover < MIN_BLKSZ) {
            blksever(bq);
            blksetsize(bp, blksize(bp) + blksize(bq)); /* Take all the space. */
            bq = blknextadj(bq);
            if (bq)
                blksetprevused(bq);
            return p;
        }

        /* Extend bp and split bq. No need to coalesce--bq is already free. */
        blksetsize(bp, gross);

        byte *nb = (byte *)bp + gross;
        blksever(bq);
        bq = blkinitfree(nb, bp->owner, leftover);
        blkprepend(bq);
        blksetprevused(bq);

        return p;
    }

    /* Make a new allocation and move the entire payload. */
    void *q = m_malloc(size);
    if (!q)
        return 0;

    memcpy(q, p, plsize(bp));
    m_free(p);

    return q;
}
