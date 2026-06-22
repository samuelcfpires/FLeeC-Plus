#pragma once

#include "config.h"


#define ALOAD(a)        __atomic_load_n(&(a), __ATOMIC_RELAXED)
#define ASTORE(a, v)    __atomic_store_n(&(a), (v), __ATOMIC_RELAXED)
/* atomic acquire load */
#define AALOAD(a)       __atomic_load_n(&(a), __ATOMIC_ACQUIRE)
/* atomic release store */
#define ARSTORE(a, v)   __atomic_store_n(&(a), (v), __ATOMIC_RELEASE)

/* atomic fetch and add */
#define ATOMIC_FADD(x, y) (__atomic_fetch_add(&(x), (y), __ATOMIC_RELAXED))
/* atomic fetch and sub */
#define ATOMIC_FSUB(x, y) (__atomic_fetch_sub(&(x), (y), __ATOMIC_RELAXED))
/* atomic add and fetch */
#define ATOMIC_ADDF(x, y) (__atomic_add_fetch(&(x), (y), __ATOMIC_RELAXED))
/* atomic sub and fetch */
#define ATOMIC_SUBF(x, y) (__atomic_sub_fetch(&(x), (y), __ATOMIC_RELAXED))
/* atomic fetch and increment */
#define ATOMIC_FINC(x)   ATOMIC_FADD(x, 1)
/* atomic fetch and decrement */
#define ATOMIC_FDEC(x)   ATOMIC_FSUB(x, 1)
/* atomic increment and fetch */
#define ATOMIC_INCF(x)   ATOMIC_ADDF(x, 1)
/* atomic decrement and fetch */
#define ATOMIC_DECF(x)   ATOMIC_SUBF(x, 1)

#define ATOMIC_ADD ATOMIC_ADDF
#define ATOMIC_SUB ATOMIC_SUBF
#define ATOMIC_INC ATOMIC_INCF
#define ATOMIC_DEC ATOMIC_DECF

#define DIV_POW2(x, y)      ((size_t)(x) >> (__builtin_ctz(y)))
#define MOD_POW2(x, y)      ((x) & ((y) - 1))

// May cause undefined behavior for negative args or when (x + y) > SIZE_MAX
#define DIV_POW2_CEIL(x, y) ((size_t)((size_t)(x) + (size_t)(y) - 1) >> (__builtin_ctz(y)))

// Align x up to next multiple of a (a must be power of 2)
#define ALIGN_UP_POW2(x, a) (((size_t)(x) + (size_t)(a) - 1) & ~((size_t)(a) - 1))
#define ALIGN_DOWN_POW2(x, a) ((size_t)(x) & ~((size_t)(a) - 1))

// Align x up to the next power of 2. Causes undefined behavior if x == 0
#define NEXT_POW2(x) (1ULL << (64 - __builtin_clzll(((size_t)(x)) - 1)))

#define IS_POW2(x) (((x) & ((x) - 1)) == 0)


/* fast-enough functions for uriencoding strings. */
void uriencode_init(void);
bool uriencode(const char *src, char *dst, const size_t srclen, const size_t dstlen);

/*
 * Wrappers around strtoull/strtoll that are safer and easier to
 * use.  For tests and assumptions, see internal_tests.c.
 *
 * str   a NULL-terminated base decimal 10 unsigned integer
 * out   out parameter, if conversion succeeded
 *
 * returns true if conversion succeeded.
 */
bool safe_strtoull(const char *str, uint64_t *out);
bool safe_strtoull_hex(const char *str, uint64_t *out);
bool safe_strtoll(const char *str, int64_t *out);
bool safe_strtoul(const char *str, uint32_t *out);
bool safe_strtol(const char *str, int32_t *out);
bool safe_strtod(const char *str, double *out);
bool safe_strcpy(char *dst, const char *src, const size_t dstmax);
bool safe_memcmp(const void *a, const void *b, size_t len);

#ifndef HAVE_HTONLL
extern uint64_t htonll(uint64_t);
extern uint64_t ntohll(uint64_t);
#endif

#ifdef __GCC
# define __gcc_attribute__ __attribute__
#else
# define __gcc_attribute__(x)
#endif

/**
 * Vararg variant of perror that makes for more useful error messages
 * when reporting with parameters.
 *
 * @param fmt a printf format
 */
void vperror(const char *fmt, ...)
    __gcc_attribute__ ((format (printf, 1, 2)));

void mc_timespec_add(struct timespec *ts1, struct timespec *ts2);


/* in case a value is needed at compile time */
#define L1_DCACHE_LINE_SIZE_DEFAULT 64

long get_l1_dcache_line_size(void);
void *l1_dcache_line_aligned_alloc(size_t size);
void *l1_dcache_line_aligned_calloc(size_t nmemb, size_t size);
