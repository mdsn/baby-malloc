#include "malloc.h"
#include <stdlib.h>

/* Define an interpose table to hijack the bindings to malloc and friends.
 * Call with
 *    DYLD_INSERT_LIBRARIES=$PWD/malloc.dylib <your-binary>
 * and it will use this malloc.
 */

#define DYLD_INTERPOSE(repl, orig) \
  __attribute__((used)) \
  static const struct { const void* newp; const void* oldp; } \
  _interpose_##orig __attribute__((section("__DATA,__interpose"))) = { \
    (const void*)&(repl), (const void*)&(orig) }

DYLD_INTERPOSE(m_malloc,  malloc);
DYLD_INTERPOSE(m_free,    free);
DYLD_INTERPOSE(m_calloc,  calloc);
DYLD_INTERPOSE(m_realloc, realloc);
