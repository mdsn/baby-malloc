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
#define MAGIC_SPENT 0xfafafafa;

enum {
    ALIGNMENT = 16,
    MINIMUM_BLKSZ = 64,     /* (?) This ought to at least be bigger than the
                             * header. */
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

/* Ensure these padded sizes are indeed multiples of ALIGNMENT, and some other
 * various aspects of layout worth knowing about.
 */
STATIC_ASSERT((SPAN_HDR_PADSZ % ALIGNMENT) == 0, span_header_size);
STATIC_ASSERT((BLOCK_HDR_PADSZ % ALIGNMENT) == 0, block_header_size);
STATIC_ASSERT(SPAN_HDR_PADSZ == 32, span_size_drifted);
STATIC_ASSERT(BLOCK_HDR_PADSZ == 48, block_size_drifted);
STATIC_ASSERT((MINIMUM_ALLOCATION & (MINIMUM_ALLOCATION - 1)) == 0, min_alloc_power_of_two);

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
    usz spsz = maxusz(gross, MINIMUM_ALLOCATION);
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
    sp->free_list->size = spsz - (usz)SPAN_HDR_PADSZ - (usz)BLOCK_HDR_PADSZ;
    sp->free_list->prev = 0;
    sp->free_list->next = 0;
    sp->free_list->owner = sp;
    sp->free_list->free = 1;
    sp->free_list->magic = MAGIC_BABY;

    return sp;
}

/* Take block bp off of its span's free list.
 */
void sever_block(struct block *bp) {
    struct span *sp = bp->owner;
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

/* Allocate the given block to serve a malloc() request. The block header is
 * already aligned, so the setup involves setting the metadata, removing the
 * block from its span's free list, and updating the span's metadata.
 */
void alloc_block(usz gross, struct block *bp) {
    /* bp points to a currently free block. Its size is bigger than or equal to
     * gross. If the remaining space after splitting is too small, take the
     * fragmentation and assign the entire block. Otherwise, split.
     */
    if (bp->size - gross < MINIMUM_BLKSZ) {
        sever_block(bp);
    } else {
        // split_block(gross, bp);
        // sever_block(bp);
    }

    /* No need to update bp's size. If it was split, split_block took care of
     * that. Otherwise, its entire size is already correct.
     */
    bp->free = 0;               /* Occupied. */
    bp->magic = MAGIC_SPENT;
    bp->prev = bp->next = 0;    /* Not strictly necessary. */
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
     * possible, sever the block from the free list, and update block and span
     * metadata.
     */
    alloc_block(gross, bp);

    /* The caller's memory comes after the block header, which is padded to
     * ALIGNMENT bytes to ensure the memory itself is aligned. The memory is
     * extended to a multiple of ALIGNMENT too, to ensure any subsequent
     * block header is automatically aligned.
     */
    return bp + 1;
}

int main(void) {
    pagesize = getpagesize();

    printf("PAGESIZE = %d\n", pagesize);
    printf("SPAN_HDR_PADSZ = %d\n", SPAN_HDR_PADSZ);
    printf("BLOCK_HDR_PADSZ = %d\n", BLOCK_HDR_PADSZ);
    printf("ALIGNMENT = %d\n", ALIGNMENT);
    printf("MINIMUM_ALLOCATION = %d\n", MINIMUM_ALLOCATION);
    printf("ALIGN_UP(128, 16) = %d\n", ALIGN_UP(128, 16));

    usz want = 128;
    usz gross = gross_size(want);

    printf("GROSS = %zu\n", gross);

    struct span *sp = alloc_span(gross);
    (void)sp;
    /* (lldb) p *sp
       (span) {
         size = 65536
         next = NULL
         free_list = 0x0000000100070020
       }
       (lldb) p *sp->free_list
       (block) {
         size = 65472
         next = NULL
         owner = 0x0000000100070000
         free = 1
         magic = 3200171710
       }
       (lldb) x sp -c 64
    sp 0x100070000: 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
                    └─ size ──────────────┘ └─ next ──────────────┘
       0x100070010: 20 00 07 00 01 00 00 00 00 00 00 00 00 00 00 00   ...............
                    └─ free_list ─────────┘ └─ padding ───────────┘
    bp 0x100070020: c0 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
                    └─ size ──────────────┘ └─ next ──────────────┘
       0x100070030: 00 00 07 00 01 00 00 00 01 00 00 00 be be be be  ................
                    └─ owner ─────────────┘ └─ free ──┘ └─ magic ─┘
    */
    return 0;
}
