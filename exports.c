#include <stddef.h>
#include <dlfcn.h> /* dlsym, RTLD_NEXT */

#include "malloc.h"     /* m_malloc et al */
#include "internal.h"   /* b32, plforeign */

/* Something inside glibc might call the internal allocator and then try to
 * free() it. Since we put our free() at the front of the loader's symbol
 * resolution order, that results in our free receiving a pointer our allocator
 * did not hand out. When this happens, detected with `plforeign()`, forward
 * the call to the next definition of `free()` below ours.
 */
static void (*_free)(void *) = 0;
static void init_fwd_free(void) {
    if (_free)
        return;

    void *sym = dlsym(RTLD_NEXT, "free");
    if (!sym) /* Not turtles all the way down */
        return;

    *(void **)(&_free) = sym;
}

static void forward_free(void *p) {
    init_fwd_free();
    _free(p);
}

/* The definitions of the actual public malloc() API.
 */
__attribute__((visibility("default")))
void *malloc(size_t s) { return m_malloc(s); }

__attribute__((visibility("default")))
void free(void *p) {
  if (plforeign(p)) {
    forward_free(p);
    return;
  }
  m_free(p);
}

__attribute__((visibility("default")))
void *calloc(size_t n, size_t s) { return m_calloc(n, s); }

__attribute__((visibility("default")))
void *realloc(void *p, size_t s) { return m_realloc(p, s); }

