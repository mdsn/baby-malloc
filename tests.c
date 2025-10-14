#include <assert.h>
#include <unistd.h> /* getpagesize */
#include <stdio.h>

#include "malloc.h"
#include "internal.h"

extern int pagesize; /* defined in malloc.c */
extern int span_count; /* defined in malloc.c */
extern struct span *base; /* defined in malloc.c */

void test_minimum_span_allocation(void);
void test_large_span_allocation(void);
void test_alloc_multiple_spans(void);
void test_free_only_span(void);
void test_free_single_block(void);
void test_blkpayload(void);
void test_block_from_payload(void);
void test_blknextadj(void);
void test_blkfoot(void);
void test_blksplit(void);
void test_isprevfree_bit(void);
void test_blkprevfoot(void);
void test_blkprevadj(void);
void test_coalesce(void);
void test_calloc(void);
void test_realloc_noalloc(void);
void test_realloc_nosize(void);
void test_realloc_truncate(void);
void test_realloc_extend_with_space(void);
void test_realloc_extend_move(void);
void test_free_unmaps_span(void);

int main(void) {
    /* malloc() calls this, so when testing helper functions it needs to be set
     * manually.
     */
    pagesize = getpagesize();

    printf("pagesize = %d\n", pagesize);
    printf("span_hdr_padsz = %d\n", SPAN_HDR_PADSZ);
    printf("block_hdr_padsz = %d\n", BLOCK_HDR_PADSZ);
    printf("alignment = %d\n", ALIGNMENT);
    printf("minimum_allocation = %d\n", MIN_MMAPSZ);
    printf("align_up(128, 16) = %d\n", ALIGN_UP(128, 16));

    test_minimum_span_allocation();
    test_large_span_allocation();
    test_free_only_span();
    test_free_single_block();
    test_blkpayload();
    test_block_from_payload();
    test_alloc_multiple_spans();
    test_blknextadj();
    test_blkfoot();
    test_blksplit();
    test_isprevfree_bit();
    test_blkprevfoot();
    test_blkprevadj();
    test_coalesce();
    test_calloc();
    test_realloc_noalloc();
    test_realloc_nosize();
    test_realloc_truncate();
    test_realloc_extend_with_space();
    test_realloc_extend_move();
    test_free_unmaps_span();

    return 0;
}

/* Get a span for a 128 bytes request. MIN_MMAPSZ (64k) gets allocated.
 * Take two blocks to serve 128 byte requests, and one large request for all the
 * rest.
 */
void test_minimum_span_allocation(void) {
    printf("==== test_minimum_span_allocation ====\n");
    usz want = 128;
    usz gross = gross_size(want);

    struct span *sp = spalloc(gross);
    assert(sp && sp->size >= gross);
    assert(!sp->prev && !sp->next);
    assert_aligned(sp->size, pagesize);
    assert(!sp->blkcount);

    struct block *bp = blkfind(gross);
    assert(bp && bp->owner == sp);
    assert(*blkfoot(bp) == blksize(bp));

    struct block *b1 = blkalloc(gross, bp);
    assert(sp->blkcount == 1);
    assert(blksize(bp) + blksize(b1) + SPAN_HDR_PADSZ == sp->size);
    assert(blkisfree(bp) && !blkisfree(b1));
    assert(blkisprevfree(b1)); /* Here prev(b1) == bp */

    struct block *b2 = blkalloc(gross, bp);
    assert(blksize(bp) + blksize(b1) + blksize(b2) + SPAN_HDR_PADSZ == sp->size);
    assert(blkisfree(bp) && !blkisfree(b2));
    assert(blkisprevfree(b2) && !blkisprevfree(b1)); /* Now prev(b1) == b2 */
    assert(sp->blkcount == 2);

    usz used = blksize(b1) + blksize(b2);
    usz rest = sp->size - SPAN_HDR_PADSZ - used;
    /* Request using up almost all free space. MIN_BLKSZ is 64, so leaving
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
    assert(sp->blkcount == 3);

    /* Clean up. */
    spfree(sp);
}

void test_large_span_allocation(void) {
    printf("==== test_large_span_allocation ====\n");
    usz want = 1024 * 1024;
    usz gross = gross_size(want);

    struct span *sp = spalloc(gross);
    assert(sp && sp->size >= gross);
    assert_aligned(sp->size, pagesize);
    assert(sp->blkcount == 0);

    /* Clean up. */
    spfree(sp);
}

void test_free_only_span(void) {
    printf("==== test_free_only_span ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    assert(sp->blkcount == 0);
    assert(sp && sp->size == MIN_MMAPSZ);
    /* sp is the only span on the global list. This actually depends on the
     * other tests cleaning up after themselves.
     */
    assert(base == sp);

    spfree(sp);

    assert(!base);
    /* sp has been munmapped--reading through it will segfault. This is an
     * artificial test because cache of SPAN_CACHE spans is kept once
     * allocated. */
}

void test_alloc_multiple_spans(void) {
    printf("==== test_alloc_multiple_spans ====\n");
    usz gross = gross_size(64);
    struct span *s1 = spalloc(gross);
    struct span *s2 = spalloc(gross);
    struct span *s3 = spalloc(gross);

    assert(s3 && base == s3); /* spalloc prepends */
    assert(s2 && s3->next == s2 && s2->prev == s3);
    assert(s1 && s2->next == s1 && s1->prev == s2);
    assert(!s3->prev && !s1->next);

    assert(s1->blkcount == 0);
    assert(s2->blkcount == 0);
    assert(s3->blkcount == 0);

    spfree(s1);
    spfree(s2);
    spfree(s3);
}

void test_free_multiple_spans(void) {
    printf("==== test_free_multiple_spans ====\n");
    usz gross = gross_size(64);
    struct span *s1 = spalloc(gross);
    struct span *s2 = spalloc(gross);
    struct span *s3 = spalloc(gross);

    /* free first span on the list */
    spfree(s3);

    assert(base == s2);
    assert(!s2->prev);

    /* free last span on the list */
    spfree(s1);
    assert(base == s2);
    assert(!s2->next);

    /* free last remaining span */
    spfree(s2);
    assert(!base);

    /* Reallocate to test removing the middle span */
    s1 = spalloc(gross);
    s2 = spalloc(gross);
    s3 = spalloc(gross);

    spfree(s2);
    assert(base == s3);
    assert(s3->next == s1 && s1->prev == s3);
    assert(!s3->prev && !s1->next);
}

void test_blkpayload(void) {
    printf("==== test_blkpayload ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);
    struct block *bp = blkfind(gross);

    char *p = blkpayload(bp);

    assert(p && (uptr)p > (uptr)bp);
    assert((uptr)p - (uptr)bp == BLOCK_HDR_PADSZ);

    spfree(sp);
}

void test_block_from_payload(void) {
    printf("==== test_block_from_payload ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);
    struct block *bp = blkfind(gross);

    char *p = blkpayload(bp);
    struct block *bq = plblk(p);

    assert(bq && bq == bp);

    spfree(sp);
}

void test_free_single_block(void) {
    printf("==== test_free_single_block ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    assert(sp && sp->size == MIN_MMAPSZ);
    assert(base == sp);

    struct block *bp = blkfind(gross);
    assert(bp && bp->owner == sp);
    assert(*blkfoot(bp) == blksize(bp));
    assert_ptr_aligned(bp, ALIGNMENT);

    struct block *b1 = blkalloc(gross, bp);
    assert(*blkfoot(bp) == blksize(bp)); /* bp shrunk */
    assert_ptr_aligned(b1, ALIGNMENT);

    assert(sp->blkcount == 1);

    /* This is the payload pointer that malloc would give to a caller.
     */
    char *p = blkpayload(b1);

    struct block *b2 = plblk(p);
    assert(b1 == b2);
    assert(!blkisfree(b2));
    assert(b2->magic == MAGIC_SPENT);

    /* This coalesces b1 into bp. */
    m_free(p);

    /* SPAN_CACHE == 1, so this span stays even though it's unused. */
    assert(sp->free_list == bp);
    assert(!bp->next);
    assert(*blkfoot(bp) == bp->owner->size - SPAN_HDR_PADSZ);

    spfree(sp);
}

/* Verify that the next block in the span can be found regardless of its
 * presence in the free list.
 */
void test_blknextadj(void) {
    printf("==== test_blknextadj ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

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

    spfree(sp);
}

void test_blkfoot(void) {
    printf("==== test_blktrail ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);

    /* The span contains: bp (free), b2 (used), b1 (used). */

    /* The foot of bp is its last usize--it should be up against b2's header.
     */
    usz *bpfoot = blkfoot(bp);
    struct block *adjbp = (struct block *)((uptr)bpfoot + sizeof(usz));
    assert(adjbp == b2);

    /* Get to b2 foot from b1 header. This only tests the blkfoot() location;
     * reading from it on an allocated block would be problematic.
     */
    usz *b2foot = blkfoot(b2);
    usz *b1prev = (usz *)((uptr)b1 - sizeof(usz));
    assert(b1prev == b2foot);

    spfree(sp);
}

void test_blksplit(void) {
    printf("==== test_blksplit ====\n");
    usz gross = gross_size(4096);
    struct span *sp = spalloc(gross);
    struct block *bp = blkfind(gross);
    struct block *b1 = blksplit(bp, gross);

    assert(b1 && blksize(b1) == gross);
    assert(blksize(bp) == sp->size - SPAN_HDR_PADSZ - gross);
    assert(*blkfoot(bp) == blksize(bp));
    assert(blkisprevfree(b1));
    /* split by itself does not change the span's blkcount */

    spfree(sp);
}

void test_isprevfree_bit(void) {
    printf("==== test_isprevfree_bit ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    /* bp -> b3 -> b2 -> b1 */
    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);
    struct block *b3 = blkalloc(gross, bp);

    assert(sp->blkcount == 3);
    assert(bp && b1 && b2 && b3);
    assert(blkisfree(bp));
    assert(!blkisfree(b3) && blkisprevfree(b3));
    assert(!blkisfree(b2) && !blkisprevfree(b2));
    assert(!blkisfree(b1) && !blkisprevfree(b1));

    blkfree(b2);

    assert(sp->blkcount == 2);
    assert(blkisfree(b2) && !blkisprevfree(b2));
    assert(!blkisfree(b1) && blkisprevfree(b1));

    spfree(sp);
}

void test_blkprevfoot(void) {
    printf("==== test_blkprevfoot ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    /* bp -> b2 -> b1 */
    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);

    assert(sp->blkcount == 2);

    blkfree(b2);
    blkfree(b1);

    assert(sp->blkcount == 0);
    assert(*blkprevfoot(b1) == blksize(b2));
    assert(*blkprevfoot(b1) == *blkfoot(b2));
    assert(*blkprevfoot(b2) == blksize(bp));
    assert(*blkprevfoot(b2) == *blkfoot(bp));

    spfree(sp);
}

void test_blkprevadj(void) {
    printf("==== test_blkprevadj ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    /* bp -> b2 -> b1 */
    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);

    assert(sp->blkcount == 2);

    blkfree(b1);
    blkfree(b2);

    assert(sp->blkcount == 0);
    assert(blkprevadj(b1) == b2);
    assert(blkprevadj(b2) == bp);
    assert(blkprevadj(bp) == 0);

    spfree(sp);
}

void test_coalesce(void) {
    printf("==== test_coalesce ====\n");
    usz gross = gross_size(64);
    struct span *sp = spalloc(gross);

    /* bp -> b3 -> b2 -> b1 */
    struct block *bp = blkfind(gross);
    struct block *b1 = blkalloc(gross, bp);
    struct block *b2 = blkalloc(gross, bp);
    struct block *b3 = blkalloc(gross, bp);

    assert(sp->blkcount == 3);

    usz bpsz = blksize(bp);

    void *p1 = blkpayload(b1);
    void *p2 = blkpayload(b2);
    void *p3 = blkpayload(b3);

    /* Should coalesce b3 and bp by extending bp (as prevadj of b3).
     * Free list after free: sp -> bp
     * Physical list: bp -> b2 -> b1
     */
    m_free(p3);
    assert(sp->free_list == bp);
    assert(blksize(bp) == bpsz + gross);
    assert(bp->next == 0);
    assert(sp->blkcount == 2);

    /* Does not coalesce -- b1 is last and b2 is in use.
     * Free list after free: sp -> b1 -> bp
     * Physical list: bp -> b2 -> b1
     */
    m_free(p1);
    assert(sp->free_list == b1);
    assert(b1->next == bp && bp->next == 0);
    assert(sp->blkcount == 1);

    /* Should coalesce everything back into bp.
     */
    m_free(p2);
    assert(sp->free_list == bp && !bp->next);
    assert(blksize(bp) == sp->size - SPAN_HDR_PADSZ);
    assert(sp->blkcount == 0);

    /* A different freeing order.
     * Physical layout: bp -> b4 -> b3 -> b2 -> b1.
     * Free b2, then b4 (coalesce with bp), then b1 (coalesce with b2), then b3
     * (coalesce everything).
     */
    b1 = blkalloc(gross, bp);
    b2 = blkalloc(gross, bp);
    b3 = blkalloc(gross, bp);
    struct block *b4 = blkalloc(gross, bp);

    assert(sp->blkcount == 4);

    bpsz = blksize(bp);

    m_free(blkpayload(b2));
    assert(blkisfree(b2) && blksize(b2) == *blkfoot(b2));
    assert(sp->free_list == b2 && b2->next == bp && !bp->next);
    assert(sp->blkcount == 3);

    m_free(blkpayload(b4));
    /* No change to the free list */
    assert(sp->free_list == b2 && b2->next == bp && !bp->next);
    assert(blksize(bp) == bpsz + gross);
    assert(blksize(bp) == *blkfoot(bp));
    assert(sp->blkcount == 2);

    m_free(blkpayload(b1));
    /* No change to the free list, but b2 changed */
    assert(sp->free_list == b2 && b2->next == bp && !bp->next);
    assert(blksize(b2) == 2 * gross);
    assert(blksize(b2) == *blkfoot(b2));
    assert(sp->blkcount == 1);

    /* Physical layout: bp (free) -> b3 (used) -> b2.
     * Free list: sp -> b2 -> bp
     */

    m_free(blkpayload(b3));
    assert(sp->free_list == bp && !bp->next);
    assert(blksize(bp) == bpsz + 4 * gross);
    assert(blksize(bp) == *blkfoot(bp));
    assert(blksize(bp) == sp->size - SPAN_HDR_PADSZ);
    assert(sp->blkcount == 0);

    spfree(sp);
}

void test_calloc(void) {
    printf("==== test_calloc ====\n");
    usz N = 1024 * 1024;
    usz SZ = sizeof(i64);

    i64 *p = m_calloc(N, SZ);

    assert(p);
    assert_ptr_aligned(p, ALIGNMENT);

    struct block *bp = plblk(p);
    struct span *sp = bp->owner;

    assert_aligned(blksize(bp), ALIGNMENT);
    assert_aligned(sp->size, pagesize);
    assert(sp->blkcount == 1);

    assert(blksize(bp) >= N * SZ);
    assert(!p[0] && !p[N - 1] && !p[1234] && !p[123456]);

    spfree(sp);
}

void test_realloc_noalloc(void) {
    printf("==== test_realloc_noalloc ====\n");
    usz size = 123;
    usz gross = gross_size(size);

    char *p = m_realloc(0, size);
    assert(p);
    assert_ptr_aligned(p, ALIGNMENT);

    struct block *bp = plblk(p);
    assert(bp);
    assert_ptr_aligned(bp, ALIGNMENT);
    assert(blksize(bp) == gross);

    struct span *sp = bp->owner;
    assert(sp->blkcount == 1);
    spfree(sp);
}

void test_realloc_nosize(void) {
    printf("==== test_realloc_nosize ====\n");
    usz size = 1234;
    usz gross = gross_size(size);

    char *p = m_malloc(size);
    assert(p);
    assert_ptr_aligned(p, ALIGNMENT);

    struct block *bp = plblk(p);
    struct span *sp = bp->owner;
    assert(sp->blkcount == 1);
    assert(bp && blksize(bp) == gross);
    assert_ptr_aligned(bp, ALIGNMENT);

    char *q = m_realloc(p, 0);
    assert(q == p); /* payload did not move */
    assert(sp->blkcount == 1);

    struct block *bq = plblk(q);
    assert(bp == bq); /* block header did not move */
    assert(blksize(bp) == MIN_BLKSZ);

    /* new block was split off of bp--big enough */
    bq = blknextadj(bq);
    assert(bq && blkisfree(bq) && !blkisprevfree(bq));
    assert(blksize(bq) == gross - blksize(bp));
    assert(sp->free_list == bq);

    spfree(sp);
}

void test_realloc_truncate(void) {
    printf("==== test_realloc_truncate ====\n");
    usz size = 1234;
    usz gross = gross_size(size);

    char *p = m_malloc(size);
    assert(p);
    assert_ptr_aligned(p, ALIGNMENT);

    struct block *bp = plblk(p);
    struct span *sp = bp->owner;
    assert(sp->blkcount == 1);
    assert(bp && blksize(bp) == gross);
    assert_ptr_aligned(bp, ALIGNMENT);

    usz nsize = 500;
    usz ngross = gross_size(nsize);
    char *q = m_realloc(p, nsize);
    assert(q == p);
    assert(sp->blkcount == 1);

    struct block *bq = plblk(q);
    assert(bp == bq);
    assert(blksize(bp) == ngross);

    bq = blknextadj(bp);
    assert(bq && blkisfree(bq) && !blkisprevfree(bq));
    assert(blksize(bq) == gross - blksize(bp));
    assert(sp->free_list == bq);

    spfree(sp);
}

void test_realloc_extend_with_space(void) {
    printf("==== test_realloc_extend_with_space ====\n");
    usz size = 1024;
    usz gross = gross_size(size);

    char *p1 = m_malloc(size);
    char *p2 = m_malloc(size);
    assert(p1 && p2);

    struct block *b1 = plblk(p1);
    struct block *b2 = plblk(p2);
    assert(b1->owner == b2->owner);
    assert(blksize(b1) == gross && blksize(b2) == gross);

    /* sp -> [free] -> b2 -> b1 */
    struct span *sp = b1->owner;
    assert(sp->blkcount == 2);
    m_free(p1); /* free the end of the span so b2 can extend in place */

    assert(sp->blkcount == 1);
    assert(sp->free_list == b1);
    assert(blknextadj(b2) == b1); /* can't use blkprevadj(b1) -- b2 is in use */
    assert(blkisfree(b1) && !blkisprevfree(b1));

    usz nsize = 1500;
    usz ngross = gross_size(nsize);

    char *q2 = m_realloc(p2, nsize);
    assert(q2 == p2);
    assert(sp->blkcount == 1);

    struct block *c2 = plblk(q2);
    assert(blksize(c2) == ngross);
    assert(b2 == c2);

    /* b1 is reduced and still in the free list */
    struct block *c1 = blknextadj(c2);
    assert(blkisfree(c1) && sp->free_list == c1);
    assert(!blkisprevfree(c1));
    /* c1 and c2 still add up to the original space of b1 and b2 */
    assert(c1 && blksize(c2) + blksize(c1) == 2*gross);

    spfree(sp);
}

void test_realloc_extend_move(void) {
    printf("==== test_realloc_extend_move ====\n");
    usz size = 1024;
    usz gross = gross_size(size);

    char *p1 = m_malloc(size);
    char *p2 = m_malloc(size);
    assert(p1 && p2);

    struct block *b1 = plblk(p1);
    struct block *b2 = plblk(p2);
    assert(b1->owner == b2->owner);

    /* sp -> [free] -> b2 -> b1 */
    struct span *sp = b1->owner;
    assert(sp->blkcount == 2);
    /* the big "antiwilderness" at the beginning of the span */
    struct block *bp = sp->free_list;

    m_free(p1); /* leave a bit over 1kb free after b2 */
    assert(sp->blkcount == 1);

    usz nsize = 4096; /* won't fit */
    usz ngross = gross_size(4096);
    char *q2 = m_realloc(p2, nsize);
    assert(q2 && q2 != p2);
    struct block *c2 = plblk(q2);

    /* b2 was freed, coalesced with b1, and put on the free list */
    assert(blkisfree(b2) && blksize(b2) == 2 * gross);
    assert(!blknextadj(b2)); /* b2 is at the end of the span now */
    assert(sp->free_list == b2);

    /* there was still enough space in sp to serve a 4kb request */
    assert(c2->owner == sp);
    assert(sp->blkcount == 1); /* realloc did not move to a new span */
    /* it happened to land right before p2, in the free space */
    assert(blknextadj(bp) == c2 && blknextadj(c2) == b2);
    assert(blkisprevfree(c2) && !blkisprevfree(b2));

    /* all span - span header - c2 - b1 - b2 */
    assert(blksize(bp) == sp->size - SPAN_HDR_PADSZ - ngross - 2 * gross);

    spfree(sp);
}

void test_free_unmaps_span(void) {
    printf("==== test_free_unmaps_span ====\n");
    /* free() will spfree() all but the last span when their count of blocks in
     * use reaches 0.
     */

    usz size = 1024;
    char *p = m_malloc(size);

    struct block *bp = plblk(p);
    struct span *sp = bp->owner;
    assert(sp->blkcount == 1);

    /* free() will not return sp because it is the only remaining span and
     * SPAN_CACHE == 1.
     */
    m_free(p);
    assert(base == sp);
    assert(sp->blkcount == 0);

    /* Request big sizes to fill them up with a single allocation. */
    size = MIN_MMAPSZ - SPAN_HDR_PADSZ - BLOCK_HDR_PADSZ;
    p = m_malloc(size);
    char *q = m_malloc(size);
    char *r = m_malloc(size);

    bp = plblk(p);
    struct block *bq = plblk(q);
    struct block *br = plblk(r);

    struct span *sq = bq->owner;
    struct span *sr = br->owner;

    /* Three different spans, sp reused. */
    assert(span_count == 3);
    assert(bp->owner == sp);
    assert(sq != sp && sr != sp && sq != sr);

    /* All three spans filled to the brim. */
    assert(!sp->free_list);
    assert(!sq->free_list);
    assert(!sr->free_list);

    m_free(r);
    assert(span_count == 2);
    m_free(q);
    assert(span_count == 1);
    m_free(p);
    assert(span_count == 1); /* kept */
    assert(base == sp);
    assert(sp->free_list);

    spfree(sp); /* manual cleanup for tests */
    assert(span_count == 0);
}
