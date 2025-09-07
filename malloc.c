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

/* Get your bearings in the debugger.
 */
#define MAGIC_BABY  0xbebebebe;

enum {
    ALIGNMENT = 16,
    MINIMUM_ALLOCATION = 64 * 1024,
};

/* If a is a power of 2, round n up to the next multiple of a.
 */
#define ALIGN_UP(n,a)   ( ((n) + (a) - 1) & ~((a) - 1) )

/* Align a pointer to the next multiple of 16 address.
 */
void *align_ptr(void *p) {
    uptr x = (uptr)p;
    uptr y = (x + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
    return (void *)y;
}

struct span {
    usz size;                   /* size including header */
    struct span *next;
    struct block *free_list;
                                /* XXX: count of blocks in use? */
};

struct block {
    usz size;                   /* size including header */
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

/* Ensure these padded sizes are indeed multiples of ALIGNMENT.
 */
STATIC_ASSERT((SPAN_HDR_PADSZ % ALIGNMENT) == 0, span_header_size);
STATIC_ASSERT((BLOCK_HDR_PADSZ % ALIGNMENT) == 0, block_header_size);

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

usz maxusz(usz a, usz b) {
    return a > b ? a : b;
}

struct span *alloc_span(usz gross) {
    usz req = maxusz(gross, MINIMUM_ALLOCATION);
    req = ALIGN_UP(req, pagesize);

    /* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
     */
    struct span *sp = mmap(0, req, PROT_WRITE | PROT_READ,
        MAP_ANON | MAP_PRIVATE, -1, 0);

    if (sp == MAP_FAILED)
        return 0;

    sp->size = req;
    sp->next = base;    /* Prepend the span to the list. */
    base = sp;

    /* Place one initial all-spanning free block immediately after the span
     * header.
     */
    struct block *bp = first_block(sp);
    bp->size = gross;
    bp->next = 0;
    bp->owner = sp;
    bp->free = 1;
    bp->magic = MAGIC_BABY;

    return sp;
}

/* Place a block in the free space of the given span, big enough to serve the
 * requested size. The setup involves placing the block header, aligned to
 * ALIGNMENT bytes, pointing it to the next piece of free space in the span, if
 * any, and updating the span values.
 */

void alloc_block(usz size, struct block *bp) {
    (void)size;
    (void)bp;
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
     * possible, splice the block from the free list, and update block and span
     * metadata.
     */
    alloc_block(size, bp);

    /* The caller's memory comes after the block header, which is padded to
     * ALIGNMENT bytes to ensure the memory itself is aligned. The memory is
     * extended to a multiple of ALIGNMENT too, to ensure any subsequent
     * block header is automatically aligned.
     */
    return bp + 1;
}

int main(void) {
    return 0;
}
