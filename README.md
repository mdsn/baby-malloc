# malloc

This repository contains a toy general purpose allocator, built on top of
`mmap(2)`. It is single threaded and supports `malloc()`, `free()`, `calloc()`
and `realloc()` with guaranteed alignment to 16 bytes.

## Build and test

Build with `make`, run tests with `make test`.

## Internals

Since `mmap(2)` provisions memory in multiples of the page size, the allocator
takes up spans whose size are multiples of the page size. Spans are organized
in a doubly linked list. Requested allocations are served as blocks from those
spans. Each span has its own internal free list of blocks.

Blocks are served from the end of a free block. When freeing, blocks are
coalesced in both directions and their payload is poisoned. Free blocks carry a
`size_t` footer indicating their size, for a quick jump to the physically
adjacent previous block.

Both span and block headers are padded to `ALIGNMENT` (16 bytes) so their
respective payloads will be aligned as well. When an allocation of some size is
requested, a real allocation is done that is padded to that alignment. This way
adjacent block headers are automatically aligned.

The minimum request for pages (via `mmap(2)`) is for 64kb. Therefore that is
the minimum span size. The minimum block size is 64 bytes. If a block split
would leave a smaller piece than that, the fragmentation is taken and the
entire space is allocated.

A cache of 1 span is kept even if it has no blocks allocated. Otherwise, when
freeing a block, if its span block count drops to 0, the span is returned with
`munmap(2)`.

### `struct span`

Span headers carry a raw `size`, `prev`/`next` pointers, a `struct block *` to
the beginning of their free list, which may be `NULL` if the entire span is in
use, and a `blkcount` that keeps track of the number of allocated blocks in the
span. All spans but the last are returned to the OS with `munmap(2)` when this
count drops to 0.

### `struct block`

Block headers carry a `size` field; since their size is a multiple of 16, the
least significant bits are used to flag free/used and (physically adjacent)
previous free/used status. The `next`/`prev` pointers are only necessary for
free blocks; their space could be made part of the payload on allocation. Block
headers have a `magic` number to help with debugging, and a pointer to their
owning span.
