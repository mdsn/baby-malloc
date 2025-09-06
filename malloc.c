#include <assert.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <inttypes.h>

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

/* If this assert fails the compiler will say something like "error:
 * 'static_assert_span_hdr_aligned' declared as an array with a negative size"
 */
#define STATIC_ASSERT(cond, name) typedef char static_assert_##name[(cond)?1:-1]

enum {
    ALIGNMENT = 16,
    MINIMUM_ALLOCATION = 64 * 1024
};

/* Poor man's alignas. A union's alignment is the maximum alignment of any
 * member. A union's size is the maximum of the sizes of its members, rounded
 * up to that alignment. The dummy _char[ALIGNMENT] forces the whole union to
 * be ALIGNMENT-aligned, and sizeof(union span) to be a multiple of ALIGNMENT.
 *
 * This ensures both types of headers respect alignment requirements. The first
 * block header after a span header is automatically aligned, and likewise the
 * memory after the block header is automatically aligned. In other words, each
 * header carries its own padding.
 */

typedef union span {
    struct {
        usz size;                   /* size including header */
        usz avail;                  /* available bytes */
        union span *next;
        union block *free_list;
                                    /* XXX: count of blocks in use? */
    } _span;
    unsigned char _align[ALIGNMENT];
} span;

STATIC_ASSERT(sizeof(union span) % ALIGNMENT == 0, span_hdr_aligned);

typedef union block {
    struct {
        usz size;                   /* size including header */
        union block *next;          /* next free block */
        union span *owner;          /* span that holds ths block */
        b32 free;                   /* is this chunk free */
        i32 magic;                  /* 0xbebebebe */
    } _block;
    unsigned char _align[ALIGNMENT];
} block;

STATIC_ASSERT(sizeof(union block) % ALIGNMENT == 0, block_hdr_aligned);

/* The initial state, when no pages have been requested from the OS, is that
 * the global base pointer is NULL. When the first request comes, the initial
 * span is tracked by this pointer.
 */
span *base = 0;

/* The page size is requested and stored here upon the first call to malloc().
 */
int pagesize = 0;

/* Align n bytes up to the next multiple of 16.
 */
usz align(usz n) {
    return (n + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/* Align a pointer to the next multiple of 16 address.
 */
void *align_ptr(void *p) {
    uptr x = (uptr)p;
    uptr y = (x + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
    return (void *)y;
}

/* mmap obtains memory in multiples of the page size, padding up the requested
 * size if necessary. Therefore it's in our best interest to round up the
 * request to a page boundary as well, to get that extra memory to ourselves.
 *
 * To minimize system calls for small allocations, a minimum allocation size of
 * MINIMUM_ALLOCATION is requested.
 */

span *alloc_span(usz size) {
}

/* Place a block in the free space of the given span, big enough to serve the
 * requested size. The setup involves placing the block header, aligned to
 * ALIGNMENT bytes, pointing it to the next piece of free space in the span, if
 * any, and updating the span values.
 */

block *alloc_block(usz size, span *s) {
}

/* Find a span with a free block big enough to serve a request. The given size
 * is the gross size--enough to hold the header and the memory.
 */
span *find_span(usz gross) { }

/* Calculate the gross size needed to serve a user request for `size` bytes.
 * The gross size includes the block header, the requested memory, and padding
 * after the memory to fill to the next ALIGNMENT boundary (so the next block
 * header will also be aligned).
 */
usz gross_size(usz size) {}

/* Serve a request for memory for the caller. Search for an already mmap'd span
 * with enough available space for the new block: its header, and the number of
 * bytes requested by the user. If one does not exist, a new span is mmap'd and
 * linked to the span list, and used to serve the request.
 */

void *malloc(usz size) {
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

    /* Try to find a span with enough space to serve the request.
     */
    span *s = find_span(gross);

    /* If no existing span has enough space to serve the request, or if there
     * is no existing span because this is the first call, a new span needs to
     * be requested from the OS.
     */
    if (s == 0) {
        s = alloc_span(gross);
        if (s == 0)     /* mmap(2) failed, not my fault */
            return 0;
    }

    /* The span at s can serve the request. Place the block header, align
     * the pointer to the memory after it and return it.
     */
    block *b = alloc_block(size, s);

    /* The caller's memory comes after the block header, which is padded to
     * ALIGNMENT bytes to ensure the memory itself is aligned. The memory is
     * extended to a multiple of ALIGNMENT too, to ensure any subsequent
     * block header is automatically aligned.
     */
    return b + 1;
}

int main(void) {
    return 0;
}
