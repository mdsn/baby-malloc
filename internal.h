#ifndef INTERNAL_H
#define INTERNAL_H

#include <inttypes.h>

#include "malloc.h"

/* See https://nullprogram.com/blog/2023/10/08/ for info on these.
 */
typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef int32_t b32;
typedef size_t usz;
typedef ptrdiff_t isz;
typedef uintptr_t uptr;
typedef unsigned char byte;

struct span {
    usz size;                   /* size including header */
    struct span *prev;
    struct span *next;
    struct block *free_list;
                                /* XXX: count of blocks in use? */
};

struct block {
    usz size;                   /* size including header */
    struct block *prev;         /* prev free block */
    struct block *next;         /* next free block */
    struct span *owner;         /* span that holds ths block */
    u32 magic;                  /* 0xbebebebe */
};

/* The block size is a multiple of ALIGNMENT = 16, so its binary representation
 * always has the 4 least significant bits set to 0. These can be used to pack
 * booleans that would otherwise consume a full word.
 */
enum {
    BIT_IN_USE = 1,
    BIT_PREV_IN_USE = 2,
    BLK_MASK = BIT_IN_USE | BIT_PREV_IN_USE,
};

/* Precomputed sizes of the headers and their padding, to be able to hop back
 * to the header from the pointer given by the caller to free().
 */
enum {
    SPAN_HDR_PADSZ = ALIGN_UP(sizeof(struct span), ALIGNMENT),
    BLOCK_HDR_PADSZ = ALIGN_UP(sizeof(struct block), ALIGNMENT),
};

/* Byte used in debugging to spot a freed block.
 */
enum {
    POISON_BYTE = 0xae,
};

/* If this assert fails the compiler will say something like "error:
 * 'static_assert_span_hdr_aligned' declared as an array with a negative size"
 */
#define STATIC_ASSERT(cond, name) typedef char static_assert_##name[(cond)?1:-1]

/* Ensure these padded sizes are indeed multiples of ALIGNMENT, and some other
 * various aspects of layout worth knowing about.
 */
STATIC_ASSERT((SPAN_HDR_PADSZ % ALIGNMENT) == 0, span_header_size);
STATIC_ASSERT((BLOCK_HDR_PADSZ % ALIGNMENT) == 0, block_header_size);
STATIC_ASSERT(SPAN_HDR_PADSZ == 32, span_size_drifted);
STATIC_ASSERT(BLOCK_HDR_PADSZ == 48, block_size_drifted);
STATIC_ASSERT((MINIMUM_ALLOCATION & (MINIMUM_ALLOCATION - 1)) == 0, min_alloc_power_of_two);

/* Internal helper functions used by malloc.c and unit tests.
 */
usz gross_size(usz size);

void assert_aligned(usz x, usz a);
void assert_ptr_aligned(void *p, usz a);

struct span *alloc_span(usz gross);
void free_span(struct span *sp);

struct block *blkalloc(usz gross, struct block *bp);
void blkfree(struct block *bp);
struct block *blkfind(usz gross);
struct block *blknextadj(struct block *bp);
struct block *blksplit(struct block *bp, usz gross);

static inline b32 blkisfree(struct block *bp) {
    return !(bp->size & BIT_IN_USE);
}
static inline void blksetfree(struct block *bp) { bp->size &= ~BIT_IN_USE; }
static inline void blksetused(struct block *bp) { bp->size |= BIT_IN_USE; }
static inline b32 blkisprevfree(struct block *bp) {
    return !(bp->size & BIT_PREV_IN_USE);
}
static inline void blksetprevfree(struct block *bp) {
    bp->size &= ~BIT_PREV_IN_USE;
}
static inline void blksetprevused(struct block *bp) {
    bp->size |= BIT_PREV_IN_USE;
}
static inline usz blkprevsize(struct block *bp) {
    return *(usz *)((uptr)bp - sizeof(usz));
}
static inline usz blksize(struct block *bp) { return bp->size & ~BLK_MASK; }
static inline void blksetsize(struct block *bp, usz size) {
    bp->size = size | (bp->size & BLK_MASK);
}
static inline usz *blkfoot(struct block *bp) {
    return (usz *)((uptr)bp + blksize(bp) - sizeof(usz));
}

struct block *block_from_payload(void *p);
void *payload_from_block(struct block *bp);

#endif
