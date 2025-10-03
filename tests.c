#include <assert.h>
#include <unistd.h> /* getpagesize */
#include <stdio.h>

#include "malloc.h"
#include "internal.h"

extern int pagesize; /* defined in malloc.c */
extern struct span *base; /* defined in malloc.c */

void test_minimum_span_allocation(void);
void test_large_span_allocation(void);
void test_alloc_multiple_spans(void);
void test_free_only_span(void);
void test_free_single_block(void);
void test_payload_from_block(void);
void test_block_from_payload(void);
void test_blknextadj(void);
void test_blkfoot(void);
void test_blksplit(void);
void test_isprevfree_bit(void);

int main(void) {
    /* malloc() calls this, so when testing helper functions it needs to be set
     * manually.
     */
    pagesize = getpagesize();

    printf("pagesize = %d\n", pagesize);
    printf("span_hdr_padsz = %d\n", SPAN_HDR_PADSZ);
    printf("block_hdr_padsz = %d\n", BLOCK_HDR_PADSZ);
    printf("alignment = %d\n", ALIGNMENT);
    printf("minimum_allocation = %d\n", MINIMUM_ALLOCATION);
    printf("align_up(128, 16) = %d\n", ALIGN_UP(128, 16));

    test_minimum_span_allocation();
    test_large_span_allocation();
    test_free_only_span();
    test_free_single_block();
    test_payload_from_block();
    test_block_from_payload();
    test_alloc_multiple_spans();
    test_blknextadj();
    test_blkfoot();
    test_blksplit();
    test_isprevfree_bit();

    return 0;
}

/* Get a span for a 128 bytes request. MINIMUM_ALLOCATION (64k) gets allocated.
 * Take two blocks to serve 128 byte requests, and one large request for all the
 * rest.
 */
void test_minimum_span_allocation(void) {
    printf("==== test_minimum_span_allocation ====\n");
    usz want = 128;
    usz gross = gross_size(want);

    struct span *sp = alloc_span(gross);
    assert(sp && sp->size >= gross);
    assert(!sp->prev && !sp->next);
    assert_aligned(sp->size, pagesize);

    struct block *bp = blkfind(gross);
    assert(bp && bp->owner == sp);
    assert(*blkfoot(bp) == blksize(bp));

    struct block *b1 = blkalloc(gross, bp);
    assert(blksize(bp) + blksize(b1) + SPAN_HDR_PADSZ == sp->size);
    assert(blkisfree(bp) && !blkisfree(b1));
    assert(blkisprevfree(b1)); /* Here prev(b1) == bp */

    struct block *b2 = blkalloc(gross, bp);
    assert(blksize(bp) + blksize(b1) + blksize(b2) + SPAN_HDR_PADSZ == sp->size);
    assert(blkisfree(bp) && !blkisfree(b2));
    assert(blkisprevfree(b2) && !blkisprevfree(b1)); /* Now prev(b1) == b2 */

    usz used = blksize(b1) + blksize(b2);
    usz rest = sp->size - SPAN_HDR_PADSZ - used;
    /* Request using up almost all free space. MINIMUM_BLKSZ is 64, so leaving
     * 24 bytes should cause the allocator to give out the entire piece. Take
     * BLOCK_HDR_PADSZ to account for gross_size() adding that to its result.
     */
    want = rest - 48 - 24;
    gross = gross_size(want);

    /* Here rest = 65152, want = 65080 and gross = 65136. gross leaves 16 bytes
     * at the end of sp, so we should get it all.
     */
    struct block *b3 = blkalloc(gross, bp);
    assert(bp == b3); /* We just got bp back */
    assert(!blkisfree(bp));
    assert(!blkisprevfree(b2) && !blkisprevfree(b1));
    assert(blksize(bp) + blksize(b1) + blksize(b2) + SPAN_HDR_PADSZ == sp->size);
    assert(!sp->free_list); /* All span is used, no more free blocks. */

    /* Clean up. */
    free_span(sp);
}

void test_large_span_allocation(void) {
    printf("==== test_large_span_allocation ====\n");
    usz want = 1024 * 1024;
    usz gross = gross_size(want);

    struct span *sp = alloc_span(gross);
    assert(sp && sp->size >= gross);
    assert_aligned(sp->size, pagesize);

    /* Clean up. */
    free_span(sp);
}

void test_free_only_span(void) {
    printf("==== test_free_only_span ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);

    assert(sp && sp->size == MINIMUM_ALLOCATION);
    /* sp is the only span on the global list. This actually depends on the
     * other tests cleaning up after themselves.
     */
    assert(base == sp);

    free_span(sp);

    assert(!base);
    /* sp has been munmapped--reading through it will segfault. */
}

void test_alloc_multiple_spans(void) {
    printf("==== test_alloc_multiple_spans ====\n");
    usz gross = gross_size(64);
    struct span *s1 = alloc_span(gross);
    struct span *s2 = alloc_span(gross);
    struct span *s3 = alloc_span(gross);

    assert(s3 && base == s3); /* alloc_span prepends */
    assert(s2 && s3->next == s2 && s2->prev == s3);
    assert(s1 && s2->next == s1 && s1->prev == s2);
    assert(!s3->prev && !s1->next);

    free_span(s1);
    free_span(s2);
    free_span(s3);
}

void test_free_multiple_spans(void) {
    printf("==== test_free_multiple_spans ====\n");
    usz gross = gross_size(64);
    struct span *s1 = alloc_span(gross);
    struct span *s2 = alloc_span(gross);
    struct span *s3 = alloc_span(gross);

    /* free first span on the list */
    free_span(s3);

    assert(base == s2);
    assert(!s2->prev);

    /* free last span on the list */
    free_span(s1);
    assert(base == s2);
    assert(!s2->next);

    /* free last remaining span */
    free_span(s2);
    assert(!base);

    /* Reallocate to test removing the middle span */
    s1 = alloc_span(gross);
    s2 = alloc_span(gross);
    s3 = alloc_span(gross);

    free_span(s2);
    assert(base == s3);
    assert(s3->next == s1 && s1->prev == s3);
    assert(!s3->prev && !s1->next);
}

void test_payload_from_block(void) {
    printf("==== test_payload_from_block ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);
    struct block *bp = blkfind(gross);

    char *p = payload_from_block(bp);

    assert(p && (uptr)p > (uptr)bp);
    assert((uptr)p - (uptr)bp == BLOCK_HDR_PADSZ);

    free_span(sp);
}

void test_block_from_payload(void) {
    printf("==== test_block_from_payload ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);
    struct block *bp = blkfind(gross);

    char *p = payload_from_block(bp);
    struct block *bq = block_from_payload(p);

    assert(bq && bq == bp);

    free_span(sp);
}

void test_free_single_block(void) {
    printf("==== test_free_single_block ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);

    assert(sp && sp->size == MINIMUM_ALLOCATION);
    assert(base == sp);

    struct block *bp = blkfind(gross);
    assert(bp && bp->owner == sp);
    assert(*blkfoot(bp) == blksize(bp));
    assert_ptr_aligned(bp, ALIGNMENT);

    struct block *b1 = blkalloc(gross, bp);
    assert(*blkfoot(bp) == blksize(bp)); /* bp shrunk */
    assert_ptr_aligned(b1, ALIGNMENT);

    /* This is the payload pointer that malloc would give to a caller.
     */
    char *p = payload_from_block(b1);

    struct block *b2 = block_from_payload(p);
    assert(b1 == b2);
    assert(!blkisfree(b2));
    assert(b2->magic == MAGIC_SPENT);

    /* Act. */
    m_free(p);

    assert(blkisfree(b2));
    assert(b2->magic == MAGIC_BABY);
    assert(sp->free_list == b2);
    assert(b2->next && b2->next == bp);
    assert(bp->prev && bp->prev == b2);
    assert(!bp->next);
    assert(!b2->prev);
    assert(*blkfoot(b2) == blksize(b2));

    free_span(sp);
}

/* Verify that the next block in the span can be found regardless of its
 * presence in the free list.
 */
void test_blknextadj(void) {
    printf("==== test_blknextadj ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);

    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);
    struct block *b3 = blkalloc(gross, bp);

    /* Now bp is in the free list, with b3, b2 and b1 immediately after it, in
     * use and in that order.
     */
    assert(blknextadj(bp) == b3);
    assert(blknextadj(b3) == b2);
    assert(blknextadj(b2) == b1);
    assert(blknextadj(b1) == 0);

    /* Put b2 at the front of the free list. This should not change the truth
     * of the assertions.
     */
    blkfree(b2);

    assert(sp->free_list == b2);
    assert(b2->next == bp);
    assert(bp->prev == b2);

    assert(blknextadj(bp) == b3);
    assert(blknextadj(b3) == b2);
    assert(blknextadj(b2) == b1);
    assert(blknextadj(b1) == 0);

    free_span(sp);
}

void test_blkfoot(void) {
    printf("==== test_blktrail ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);

    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);

    /* The span contains: bp (free), b2 (used), b1 (used). */

    /* The foot of bp is its last usize--it should be up against b2's header.
     */
    usz *bpfoot = blkfoot(bp);
    struct block *adjbp = (struct block *)((uptr)bpfoot + sizeof(usz));
    assert(adjbp == b2);

    /* Get to b2 foot from b1 header.
     */
    usz *b2foot = blkfoot(b2);
    usz *b1prev = (usz *)((uptr)b1 - sizeof(usz));
    assert(b1prev == b2foot);

    free_span(sp);
}

void test_blksplit(void) {
    printf("==== test_blksplit ====\n");
    usz gross = gross_size(4096);
    struct span *sp = alloc_span(gross);
    struct block *bp = blkfind(gross);

    struct block *b1 = blksplit(bp, gross);

    assert(b1 && blksize(b1) == gross);
    assert(blksize(bp) == sp->size - SPAN_HDR_PADSZ - gross);
    assert(*blkfoot(bp) == blksize(bp));
    assert(blkisprevfree(b1));

    free_span(sp);
}

void test_isprevfree_bit(void) {
    printf("==== test_isprevfree_bit ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);

    /* bp -> b3 -> b2 -> b1 */
    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);
    struct block *b3 = blkalloc(gross, bp);

    assert(bp && b1 && b2 && b3);
    assert(blkisfree(bp));
    assert(!blkisfree(b3) && blkisprevfree(b3));
    assert(!blkisfree(b2) && !blkisprevfree(b2));
    assert(!blkisfree(b1) && !blkisprevfree(b1));

    blkfree(b2);

    assert(blkisfree(b2) && !blkisprevfree(b2));
    assert(!blkisfree(b1) && blkisprevfree(b1));

    free_span(sp);
}
