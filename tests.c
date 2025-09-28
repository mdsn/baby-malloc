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

    struct block *bp = find_block(gross);
    assert(bp->owner == sp);

    struct block *b1 = alloc_block(gross, bp);
    assert(bp->size + b1->size + SPAN_HDR_PADSZ == sp->size);
    assert(bp->free && !b1->free);

    struct block *b2 = alloc_block(gross, bp);
    assert(bp->size + b1->size + b2->size + SPAN_HDR_PADSZ == sp->size);
    assert(bp->free && !b2->free);

    usz used = b1->size + b2->size;
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
    struct block *b3 = alloc_block(gross, bp);
    assert(bp == b3); /* We just got bp back */
    assert(!bp->free);
    assert(bp->size + b1->size + b2->size + SPAN_HDR_PADSZ == sp->size);
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
    struct block *bp = find_block(gross);

    char *p = payload_from_block(bp);

    assert(p && (uptr)p > (uptr)bp);
    assert((uptr)p - (uptr)bp == BLOCK_HDR_PADSZ);

    free_span(sp);
}

void test_block_from_payload(void) {
    printf("==== test_block_from_payload ====\n");
    usz gross = gross_size(64);
    struct span *sp = alloc_span(gross);
    struct block *bp = find_block(gross);
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

    struct block *bp = find_block(gross);
    assert(bp && bp->owner == sp);
    assert_ptr_aligned(bp, ALIGNMENT);

    struct block *b1 = alloc_block(gross, bp);
    assert_ptr_aligned(b1, ALIGNMENT);

    /* This is the payload pointer that malloc would give to a caller.
     */
    char *p = payload_from_block(b1);

    /* TODO test block_from_payload() and its inverse separately.
     */
    struct block *b2 = block_from_payload(p);
    assert(b1 == b2);
    assert(!b2->free);
    assert(b2->magic == MAGIC_SPENT);

    /* Act. */
    m_free(p);

    assert(b2->free);
    assert(b2->magic == MAGIC_BABY);
    assert(sp->free_list == b2);
    assert(b2->next && b2->next == bp);
    assert(bp->prev && bp->prev == b2);
    assert(!bp->next);
    assert(!b2->prev);

    free_span(sp);
}
