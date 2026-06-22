#ifndef ASSOC_BUCKET_H
#define ASSOC_BUCKET_H

#include "memcached.h"
#include "recl.h"

// needs to be visible for assoc_bucket.c
// cannot be uint because one thread might add and other remove
typedef struct {
    int64_t value;
    char padding[L1_DCACHE_LINE_SIZE_DEFAULT - sizeof(int64_t)];
} __attribute__((aligned(L1_DCACHE_LINE_SIZE_DEFAULT))) curr_items_t;

_Static_assert(sizeof(curr_items_t) == L1_DCACHE_LINE_SIZE_DEFAULT, "curr_items_t size must be equal to L1_DCACHE_LINE_SIZE_DEFAULT");

//Amount of collision lists
#define hashsize(n) ((uint64_t)1<<(n))

//Masks to know to what collision list an item goes to
#define hashmask(n) (hashsize(n)-1)


#define HV_16MSB(hv) ((uint16_t)((uint64_t)(hv) >> 48))


typedef struct
array_entry {
    intptr_t it : 48;
    uint16_t hv : 16;
} array_entry_t;

_Static_assert(sizeof(array_entry_t) == 8, "array_entry_t size must be 8 bytes");

typedef struct bucket_array {
    size_t len;
#ifdef ASSOC_BUCKET_ARENA_GLOBAL
    struct arena* arena;
#endif
    array_entry_t entries[];
} bucket_array_t;

struct arena;

typedef struct bucket {
    bucket_array_t* arr; // atomic ptr
#if defined(ASSOC_BUCKET_ARENA_PERBUCKET) || defined(ASSOC_BUCKET_ARENA_GLOBAL)
    struct arena* arena; // atomic ptr
#endif
} bucket_t;

#ifdef RECL_EBR
item* get_ebr(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv_msb);
#define get get_ebr
#elif defined(RECL_HP)
item* get_hp(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv);
#define get get_hp
#elif defined(RECL_QSENSE)
item* get_ebr(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv_msb);
item* get_hp(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv);
item* get_qsense(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv);
#define get get_qsense
#else
#error "Must define a reclamation scheme."
#endif


item* insert(bucket_t* bucket, const item *it, const uint16_t hv_msb);
bool del(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv_msb, const bool reclaim);
int replace(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv_msb, const item *new_it);
int set(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv_msb, const item *new_it);
/* Clears a bucket and returns the number of freed items */
int clear(bucket_t* bucket, size_t* freed_bytes);

void assoc_bucket_init(size_t arena_size);
bucket_t* new_bucket(void);
/* Migrates items that would hash to different buckets after expansion */
void migrate_expanded_items(bucket_t** old_hashtable, bucket_t** new_hashtable, const uint64_t old_hashpower, const uint64_t new_hashpower);

static inline bool is_empty(bucket_t* bucket) {
    return AALOAD(bucket->arr)->len == 0;
}

#endif // ASSOC_BUCKET_H
