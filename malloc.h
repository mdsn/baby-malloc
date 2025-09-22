#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

/* See https://nullprogram.com/blog/2023/10/08/
 * #define assert(c)  while (!(c)) __builtin_unreachable()
 */

/* Get your bearings in the debugger.
 */
#define MAGIC_BABY  0xbebebebe
#define MAGIC_SPENT 0xdededede

enum {
    ALIGNMENT = 16,
    MINIMUM_BLKSZ = 64,     /* (?) This ought to at least be bigger than the
                             * header. */
    MINIMUM_ALLOCATION = 64 * 1024,
};

/* If a is a power of 2, round n up to the next multiple of a.
 */
#define ALIGN_UP(n,a)   ( ((n) + (a) - 1) & ~((a) - 1) )

/* Opaque types to avoid exposing layout.
 */
struct span;
struct block;

/* Public API, prefixed with m_ for now.
 */
void *m_malloc(size_t n);
void m_free(void *p);

#endif
