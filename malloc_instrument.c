#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <execinfo.h>

//
// This LD_PRELOAD library instruments malloc, calloc, realloc, memalign,
// valloc, posix_memalign, and free by outputting all calls to stderr.
//
// Unfortunately, it's not quite as straightforward as it sounds, as fprintf
// and dlsym both use heap-based memory allocation. During initialization, we
// use a dummy implementation of malloc that uses a small buffer. Once dlsym
// loading is done, then we switch to our real implementation, which, unless
// a recursive mutex is already held, first outputs the call arguments, makes
// the call, and then outputs the return value. If the recursive mutex is
// already held, then the call was due to some call made while outputting
// arguments, so we just forward the call along to the real call.
//
// Parsing this output can be done by the corresponding utility Python script.
//
// Big thanks to http://stackoverflow.com/a/10008252/379568
//
// -JT Olds <jt@spacemonkey.com>
//

// gcc -shared -fPIC -o malloc_instrument.so malloc_instrument.c -lpthread -ldl

#define OUTPUT_PREFIX "|||||||||||||||||||||| "

static void* (*real_malloc)(size_t size);
static void* (*real_calloc)(size_t nmemb, size_t size);
static void* (*real_realloc)(void *ptr, size_t size);
static void* (*real_memalign)(size_t blocksize, size_t bytes);
static void* (*real_valloc)(size_t size);
static int   (*real_posix_memalign)(void** memptr, size_t alignment,
                                     size_t size);
static void  (*real_free)(void *ptr);
static void* (*temp_malloc)(size_t size);
static void* (*temp_calloc)(size_t nmemb, size_t size);
static void* (*temp_realloc)(void *ptr, size_t size);
static void* (*temp_memalign)(size_t blocksize, size_t bytes);
static void* (*temp_valloc)(size_t size);
static int   (*temp_posix_memalign)(void** memptr, size_t alignment,
                                     size_t size);
static void  (*temp_free)(void *ptr);

pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t internal_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutexattr_t internal_mutex_attr;
int initializing = 0;
int initialized = 0;
int internal = 0;

char tmpbuf[1024];
unsigned long tmppos = 0;
unsigned long tmpallocs = 0;

void* dummy_malloc(size_t size) {
    if (tmppos + size >= sizeof(tmpbuf)) exit(1);
    void *retptr = tmpbuf + tmppos;
    tmppos += size;
    ++tmpallocs;
    return retptr;
}

void* dummy_calloc(size_t nmemb, size_t size) {
    void *ptr = dummy_malloc(nmemb * size);
    unsigned int i = 0;
    for (; i < nmemb * size; ++i)
        *((char*)(ptr + i)) = '\0';
    return ptr;
}

void dummy_free(void *ptr) {
}

void dump_prefix() {
    // IMPORTANT: if you change where this is called from, you may need to
    // adjust the depth so that we get the correct caller.
    //
    // TODO: detect our module address and search backwards until we hit
    // somebody else
    const size_t our_depth = 2;
    const size_t caller_depth = our_depth + 1;
    void *array[caller_depth];
    size_t size;
    char **caller;
    size_t i;

    size = backtrace (array, caller_depth);
    if (size > our_depth) {
      caller = backtrace_symbols (array + our_depth, 1);
      fprintf(stderr, OUTPUT_PREFIX "%s: ", *caller);
      free (caller);
    } else {
      fprintf(stderr, OUTPUT_PREFIX "err: ");
    }

}

int start_call() {
    pthread_mutex_lock(&init_mutex);
    if (!initializing) {
        initializing = 1;
        pthread_mutexattr_init(&internal_mutex_attr);
        pthread_mutexattr_settype(&internal_mutex_attr,
                PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&internal_mutex, &internal_mutex_attr);
        pthread_mutex_lock(&internal_mutex);
        pthread_mutex_unlock(&init_mutex);

        real_malloc         = dummy_malloc;
        real_calloc         = dummy_calloc;
        real_realloc        = NULL;
        real_free           = dummy_free;
        real_memalign       = NULL;
        real_valloc         = NULL;
        real_posix_memalign = NULL;

        temp_malloc         = dlsym(RTLD_NEXT, "malloc");
        temp_calloc         = dlsym(RTLD_NEXT, "calloc");
        temp_realloc        = dlsym(RTLD_NEXT, "realloc");
        temp_free           = dlsym(RTLD_NEXT, "free");
        temp_memalign       = dlsym(RTLD_NEXT, "memalign");
        temp_valloc         = dlsym(RTLD_NEXT, "valloc");
        temp_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");

        if (!temp_malloc || !temp_calloc || !temp_realloc || !temp_memalign ||
            !temp_valloc || !temp_posix_memalign || !temp_free)
        {
            fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
            exit(1);
        }

        real_malloc         = temp_malloc;
        real_calloc         = temp_calloc;
        real_realloc        = temp_realloc;
        real_free           = temp_free;
        real_memalign       = temp_memalign;
        real_valloc         = temp_valloc;
        real_posix_memalign = temp_posix_memalign;

        initialized = 1;
    } else {
        pthread_mutex_unlock(&init_mutex);
        pthread_mutex_lock(&internal_mutex);
    }

    if (!initialized || internal) {
        pthread_mutex_unlock(&internal_mutex);
        return 1;
    }
    internal = 1;
    return 0;
}

void end_call() {
    internal = 0;
    pthread_mutex_unlock(&internal_mutex);
}

void* malloc(size_t size) {
    if (start_call()) return real_malloc(size);
    void *rv = NULL;
    dump_prefix();
    fprintf(stderr, "malloc(%zu) = ", size);
    rv = real_malloc(size);
    fprintf(stderr, "%p\n", rv);
    end_call();

    return rv;
}

void* calloc(size_t nmemb, size_t size) {
    if (start_call()) return real_calloc(nmemb, size);
    void *p = NULL;
    dump_prefix();
    fprintf(stderr, "calloc(%zu, %zu) = ", nmemb, size);
    p = real_calloc(nmemb, size);
    fprintf(stderr, "%p\n", p);
    end_call();
    return p;
}

void* realloc(void *ptr, size_t size) {
    if (start_call()) return real_realloc(ptr, size);
    void *p = NULL;
    dump_prefix();
    fprintf(stderr, "realloc(%p, %zu) = ", ptr, size);
    p = real_realloc(ptr, size);
    fprintf(stderr, "%p\n", p);
    end_call();
    return p;
}

void free(void *ptr) {
    if (start_call()) {
        real_free(ptr);
        return;
    }

    dump_prefix();
    fprintf(stderr, "free(%p)\n", ptr);
    real_free(ptr);
    end_call();

    return;
}

void* memalign(size_t blocksize, size_t bytes) {
    if (start_call()) return real_memalign(blocksize, bytes);

    void *p = NULL;
    dump_prefix();
    fprintf(stderr, "memalign(%zu, %zu) = ", blocksize,
            bytes);
    p = real_memalign(blocksize, bytes);
    fprintf(stderr, "%p\n", p);
    end_call();
    return p;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (start_call()) return real_posix_memalign(memptr, alignment, size);

    int rv = 0;
    dump_prefix();
    fprintf(stderr, "posix_memalign(%p, %zu, %zu) = ",
            memptr, alignment, size);
    rv = real_posix_memalign(memptr, alignment, size);
    if (rv == 0) {
        fprintf(stderr, "0, %p\n", *memptr);
    } else {
        fprintf(stderr, "%d, NULL\n", rv);
    }
    end_call();
    return rv;
}

void* valloc(size_t size) {
    if (start_call()) return real_valloc(size);

    void *p = NULL;
    dump_prefix();
    fprintf(stderr, "valloc(%zu) = ", size);
    p = real_valloc(size);
    fprintf(stderr, "%p\n", p);
    end_call();
    return p;
}
