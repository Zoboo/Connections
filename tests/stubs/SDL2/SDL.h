#pragma once
#include <ctype.h>
#include <stddef.h>
typedef struct { int value; } SDL_atomic_t;
typedef struct { int unused; } SDL_Thread;
static SDL_Thread test_thread;
static int test_threads_started;
static int SDL_AtomicGet(SDL_atomic_t *value) { return value->value; }
static int SDL_AtomicSet(SDL_atomic_t *value, int next) { int old = value->value; value->value = next; return old; }
static SDL_Thread *SDL_CreateThreadWithStackSize(int (*fn)(void *), const char *name, size_t size, void *arg) {
    // Tests deliver each completed job explicitly; never perform network I/O.
    (void)fn; (void)name; (void)size; (void)arg; test_threads_started++; return &test_thread;
}
static void SDL_WaitThread(SDL_Thread *thread, int *result) { (void)thread; if (result) *result = 0; }
static int SDL_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int diff = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (diff || !a[i]) return diff;
    }
    return 0;
}
