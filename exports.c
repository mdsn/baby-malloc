#include <stddef.h>

void *m_malloc(size_t s);
void m_free(void *p);
void *m_calloc(size_t n, size_t s);
void *m_realloc(void *p, size_t s);

__attribute__((visibility("default")))
void *malloc(size_t s) { return m_malloc(s); }

__attribute__((visibility("default")))
void free(void *p) { m_free(p); }

__attribute__((visibility("default")))
void *calloc(size_t n, size_t s) { return m_calloc(n, s); }

__attribute__((visibility("default")))
void *realloc(void *p, size_t s) { return m_realloc(p, s); }
