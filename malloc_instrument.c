#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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

// gcc -shared -fPIC -o malloc_instrument.so malloc_instrument.c -ldl

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

__thread unsigned int entered = 0;

int start_call() {
  return __sync_fetch_and_add(&entered, 1);
}

void end_call() {
  __sync_fetch_and_sub(&entered, 1);
}

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

void __attribute__((constructor)) hookfns() {
    start_call();
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
    end_call();
}



typedef struct record_tag {
  enum alloc_type {
    MALLOC_CALL,
    CALLOC_CALL,
    REALLOC_CALL,
    MEMALIGN_CALL,
    VALLOC_CALL,
    POSIX_MEMALIGN_CALL,
    FREE_CALL
  } type;

  union {
    struct {
      size_t size;
      void* ptr;
    } malloc_call;

    struct {
      size_t nmemb;
      size_t size;
      void* ptr;
    } calloc_call;

    struct {
      void* in_ptr;
      size_t size;
      void* out_ptr;
    } realloc_call;

    struct {
      size_t blocksize;
      size_t bytes;
      void* ptr;
    } memalign_call;

    struct {
      size_t size;
      void* ptr;
    } valloc_call;

    struct {
      void** memptr;
      size_t alignment;
      size_t size;
      int rv;
      void* ptr;
    } posix_memalign_call;

    struct {
      void* ptr;
    } free_call;
  };
} call_record;

void* get_caller() {
    const size_t our_depth = 3;
    const size_t caller_depth = our_depth + 1;
    void *array[caller_depth];
    size_t size = backtrace(array, caller_depth);
    if (size > our_depth) {
        return array[our_depth];
    }
    return NULL;
}

void do_call(call_record *record) {
    int internal = 0;
    char **symbol = NULL;
    char *caller = NULL;

    internal = start_call();

    if (!internal) {
        void* calling_func = get_caller();
        if (calling_func) {
            symbol = backtrace_symbols(&calling_func, 1);
            if (symbol) {
                caller = *symbol;
            }
        }
    }

    if (!caller) {
        caller = "UNK";
    }

#define DUMPLINE(fmt, ...) if (!internal) fprintf(stderr, \
        "|||||||||||||||||||||| %s: " fmt "\n", caller, __VA_ARGS__);

    switch(record->type) {
        case MALLOC_CALL:
        record->malloc_call.ptr = real_malloc(record->malloc_call.size);
        DUMPLINE("malloc(%zu) = %p",
                record->malloc_call.size,
                record->malloc_call.ptr);
        break;
        case CALLOC_CALL:
        record->calloc_call.ptr = real_calloc(
                record->calloc_call.nmemb,
                record->calloc_call.size);
        DUMPLINE("calloc(%zu, %zu) = %p",
                record->calloc_call.nmemb,
                record->calloc_call.size,
                record->calloc_call.ptr);
        break;
        case REALLOC_CALL:
        record->realloc_call.out_ptr = real_realloc(
                record->realloc_call.in_ptr,
                record->realloc_call.size);
        DUMPLINE("realloc(%p, %zu) = %p",
                record->realloc_call.in_ptr,
                record->realloc_call.size,
                record->realloc_call.out_ptr);
        break;
        case MEMALIGN_CALL:
        record->memalign_call.ptr = real_memalign(
                record->memalign_call.blocksize,
                record->memalign_call.bytes);
        DUMPLINE("memalign(%zu, %zu) = %p",
                record->memalign_call.blocksize,
                record->memalign_call.bytes,
                record->memalign_call.ptr);
        break;
        case VALLOC_CALL:
        record->valloc_call.ptr = real_valloc(
                record->valloc_call.size);
        DUMPLINE("valloc(%zu) = %p",
                record->valloc_call.size,
                record->valloc_call.ptr);
        break;
        case POSIX_MEMALIGN_CALL:
        record->posix_memalign_call.rv = real_posix_memalign(
                record->posix_memalign_call.memptr,
                record->posix_memalign_call.alignment,
                record->posix_memalign_call.size);
        if (record->posix_memalign_call.rv == 0) {
            DUMPLINE("posix_memalign(%p, %zu, %zu) = 0, %p",
                    record->posix_memalign_call.memptr,
                    record->posix_memalign_call.alignment,
                    record->posix_memalign_call.size,
                    *record->posix_memalign_call.memptr);
        } else {
            DUMPLINE("posix_memalign(%p, %zu, %zu) = %d, NULL",
                    record->posix_memalign_call.memptr,
                    record->posix_memalign_call.alignment,
                    record->posix_memalign_call.size,
                    record->posix_memalign_call.rv);
        }
        break;
        case FREE_CALL:
        real_free(record->free_call.ptr);
        DUMPLINE("free(%p)", record->free_call.ptr);
        break;
    };

    if (symbol) {
        free(symbol);
    }

    end_call();
}

void* malloc(size_t size) {
    call_record record;
    record.type = MALLOC_CALL;
    record.malloc_call.size = size;
    do_call(&record);

    return record.malloc_call.ptr;
}

void* calloc(size_t nmemb, size_t size) {
    call_record record;
    record.type = CALLOC_CALL;
    record.calloc_call.nmemb = nmemb;
    record.calloc_call.size = size;
    do_call(&record);

    return record.calloc_call.ptr;
}

void* realloc(void *ptr, size_t size) {
    call_record record;
    record.type = REALLOC_CALL;
    record.realloc_call.in_ptr = ptr;
    record.realloc_call.size = size;
    do_call(&record);

    return record.realloc_call.out_ptr;
}

void free(void *ptr) {
    call_record record;
    record.type = FREE_CALL;
    record.free_call.ptr = ptr;
    do_call(&record);
}

void* memalign(size_t blocksize, size_t bytes) {
    call_record record;
    record.type = MEMALIGN_CALL;
    record.memalign_call.blocksize = blocksize;
    record.memalign_call.bytes = bytes;
    do_call(&record);

    return record.memalign_call.ptr;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    call_record record;
    record.type = MEMALIGN_CALL;
    record.posix_memalign_call.memptr = memptr;
    record.posix_memalign_call.alignment = alignment;
    record.posix_memalign_call.size = size;
    do_call(&record);

    return record.posix_memalign_call.rv;
}

void* valloc(size_t size) {
    call_record record;
    record.type = VALLOC_CALL;
    record.valloc_call.size = size;
    do_call(&record);

    return record.valloc_call.ptr;
}
