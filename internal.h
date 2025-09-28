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
    b32 free;                   /* is this chunk free */
                                /* XXX use size lowest bits for this */
    u32 magic;                  /* 0xbebebebe */
};

/* Precomputed sizes of the headers and their padding, to be able to hop back
 * to the header from the pointer given by the caller to free().
 */
enum {
    SPAN_HDR_PADSZ = ALIGN_UP(sizeof(struct span), ALIGNMENT),
    BLOCK_HDR_PADSZ = ALIGN_UP(sizeof(struct block), ALIGNMENT),
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

struct block *find_block(usz gross);
struct block *alloc_block(usz gross, struct block *bp);

struct block *block_from_payload(void *p);
void *payload_from_block(struct block *bp);

#endif
