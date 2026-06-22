#ifndef USE_ASSOC_NBLIST

/**
 * Immutable bucket array implementation.
 */

#include <stdint.h>
#include "assoc_bucket.h"
#include "recl.h"
#include "util.h"

// TODO: these defines are very badly done after having to switch the default version
// This needs to be refactored or deleted, but for now
#if !defined(ASSOC_BUCKET_ARENA_GLOBAL) && !defined(ASSOC_BUCKET_ARENA_PERBUCKET) && !defined(ASSOC_BUCKET_ARENA_MALLOC)
// Use memory allocator by default
#define ASSOC_BUCKET_ARENA_MALLOC
#endif

#if !defined(ASSOC_BUCKET_ARENA_MALLOC) && !defined(RECL_EBR)
#error "Arena allocation incompatible with non-EBR reclamation."
#endif

#ifdef ASSOC_BUCKET_ARENA_MALLOC
#include <malloc.h>
#endif


static bucket_array_t empty_bucket_array;


#define AE_LEN_TO_SIZE(len) (sizeof(bucket_array_t) + (len) * sizeof(array_entry_t))

#define AE_ITEM(e) ((item*)(intptr_t)(e.it))

#ifdef ASSOC_BUCKET_NO_HASH
#define key_equals(e, key, nkey, hv) (!strncmp(ITEM_key(AE_ITEM(e)), (key), (nkey)))
#else
#define key_equals(e, key, nkey, hv) \
    ((uint16_t)((e).hv) == (uint16_t)(hv) && !strncmp(ITEM_key(AE_ITEM(e)), (key), (nkey)))
#endif

#define CAS(addr, oldval, newval) \
    __atomic_compare_exchange_n((addr), (oldval), (newval), false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)


#ifndef ASSOC_BUCKET_ARENA_MALLOC
static void arena_init(size_t arena_size);
static struct arena* new_arena(void);
static bucket_array_t* arena_alloc(bucket_t* bucket, size_t size);
#endif

#ifdef ASSOC_BUCKET_ARENA_MALLOC_FORCE_ALIGN
#define my_aligned_alloc(alignment, size) aligned_alloc(alignment, ALIGN_UP_POW2(size, alignment))
#else
#define my_aligned_alloc(alignment, size) malloc(size)
#endif

static bucket_array_t* alloc_bucket_array(bucket_t* bucket, size_t len) {
    size_t size = AE_LEN_TO_SIZE(len);
    bucket_array_t* arr;
#if defined(ASSOC_BUCKET_ARENA_MALLOC) && defined(ASSOC_BUCKET_ARENA_NO_PADDING)
    arr = malloc(size);
#elif defined(ASSOC_BUCKET_ARENA_MALLOC)
    if (__glibc_likely(size <= L1_DCACHE_LINE_SIZE_DEFAULT)) {
        // This branch should always be taken
        arr = my_aligned_alloc(L1_DCACHE_LINE_SIZE_DEFAULT, L1_DCACHE_LINE_SIZE_DEFAULT);
    } else {
        arr = my_aligned_alloc(L1_DCACHE_LINE_SIZE_DEFAULT, ALIGN_UP_POW2(size, L1_DCACHE_LINE_SIZE_DEFAULT));
    }
#else
    arr = arena_alloc(bucket, size);
#endif
    arr->len = len;
    return arr;
}

#ifdef ASSOC_BUCKET_ARENA_MALLOC
static inline void free_bucket_array(bucket_array_t* arr) {
    if (!arr || arr == &empty_bucket_array)
        return;

    recl_retire_mem(arr);
}
#define ASSOC_BUCKET_MALLOC_FREE(arr) free_bucket_array(arr)
#else
#define ASSOC_BUCKET_MALLOC_FREE(arr)
#endif

static bucket_array_t* realloc_bucket_array(bucket_t* bucket, bucket_array_t* old_arr, size_t new_len) {
    if (!old_arr) {
        return alloc_bucket_array(bucket, new_len);
    }

    size_t old_len = old_arr->len;

#if defined(ASSOC_BUCKET_ARENA_MALLOC) && defined(ASSOC_BUCKET_ARENA_NO_PADDING)
    if (new_len <= old_len || AE_LEN_TO_SIZE(new_len) <= malloc_usable_size(old_arr)) {
#else
    size_t new_size;
    if (new_len <= old_len
        || __glibc_likely((new_size = AE_LEN_TO_SIZE(new_len)) <= L1_DCACHE_LINE_SIZE_DEFAULT)
        || new_size <= ALIGN_UP_POW2(AE_LEN_TO_SIZE(old_len), L1_DCACHE_LINE_SIZE_DEFAULT)
#ifdef ASSOC_BUCKET_ARENA_MALLOC
        || malloc_usable_size(old_arr) >= new_size
#endif
    ) {
#endif
        old_arr->len = new_len;
        return old_arr;
    } else {
        ASSOC_BUCKET_MALLOC_FREE(old_arr);
        return alloc_bucket_array(bucket, new_len);
    }
}

__attribute__((always_inline))
static inline bucket_array_t* realloc_bucket_array_del(bucket_t* bucket, bucket_array_t* old_arr, size_t new_len) {
    if (new_len == 0) {
        ASSOC_BUCKET_MALLOC_FREE(old_arr);
        return &empty_bucket_array;
    }

    return realloc_bucket_array(bucket, old_arr, new_len);
}

static ssize_t search_index(bucket_array_t* arr, const char* search_key, const size_t nkey, const uint16_t hv) {
    for (ssize_t i = 0; i < arr->len; i++) {
        array_entry_t e = arr->entries[i];
        if (key_equals(e, search_key, nkey, hv)) {
            return i;
        }
    }

    return -1;
}

item* insert(bucket_t* bucket, const item* it, const uint16_t hv) {
    bucket_array_t* arr;
    bucket_array_t* new_arr = NULL;
    item* it_found;

retry:
    it_found = get(bucket, ITEM_key(it), it->nkey, hv);
    if (it_found != NULL) {
        ASSOC_BUCKET_MALLOC_FREE(new_arr);
        return it_found;
    }

    arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

    size_t new_len = arr->len + 1;
    if (!new_arr || new_arr->len < new_len) {
        new_arr = realloc_bucket_array(bucket, new_arr, new_len);
    } else {
        new_arr->len = new_len;
    }

    memcpy(new_arr->entries, arr->entries, arr->len * sizeof(array_entry_t));

    new_arr->entries[arr->len] = (array_entry_t){.it = (intptr_t)it, .hv = hv};

    if (!CAS(&bucket->arr, &arr, new_arr)) {
        recl_unprotect_pop_mem();
        goto retry;
    }

    recl_unprotect_pop_mem();

    ASSOC_BUCKET_MALLOC_FREE(arr);

    return NULL;
}

bool del(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv, bool reclaim) {
    bucket_array_t* arr;
    bucket_array_t* new_arr = NULL;
    ssize_t idx;

retry:
    arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

    idx = search_index(arr, search_key, nkey, hv);
    if (idx == -1) {
        recl_unprotect_pop_mem();
        ASSOC_BUCKET_MALLOC_FREE(new_arr);
        return false;
    }

    size_t new_len = arr->len - 1;
    if (!new_arr || new_arr->len < new_len) {
        new_arr = realloc_bucket_array_del(bucket, new_arr, new_len);
    } else {
        new_arr->len = new_len;
    }

    memcpy(new_arr->entries, arr->entries, idx * sizeof(array_entry_t));
    memcpy(&new_arr->entries[idx], &arr->entries[idx + 1], (arr->len - idx - 1) * sizeof(array_entry_t));

    if (!CAS(&bucket->arr, &arr, new_arr)) {
        recl_unprotect_pop_mem();
        goto retry;
    }

    recl_unprotect_pop_mem();

    if(reclaim) {
        recl_retire_item(AE_ITEM(arr->entries[idx]));
    }

    ASSOC_BUCKET_MALLOC_FREE(arr);

    return true;
}

int replace(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv, const item* new_it) {
    bucket_array_t* arr;
    bucket_array_t* new_arr = NULL;
    ssize_t idx;

retry:
    arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

    idx = search_index(arr, search_key, nkey, hv);
    if (idx == -1) {
        recl_unprotect_pop_mem();
        ASSOC_BUCKET_MALLOC_FREE(new_arr);
        return 0;
    }

    size_t new_len = arr->len;
    if (!new_arr || new_arr->len < new_len) {
        new_arr = realloc_bucket_array(bucket, new_arr, new_len);
    } else {
        new_arr->len = new_len;
    }

    memcpy(new_arr->entries, arr->entries, arr->len * sizeof(array_entry_t));
    new_arr->entries[idx] = (array_entry_t){.it = (intptr_t)new_it, .hv = hv};

    if (!CAS(&bucket->arr, &arr, new_arr)) {
        recl_unprotect_pop_mem();
        goto retry;
    }

    recl_unprotect_pop_mem();

    // no need to protect item since we're the only ones who can free it
    item* old_it = AE_ITEM(arr->entries[idx]);
    int old_it_nbytes = old_it->nbytes;
    recl_retire_item(old_it);
    ASSOC_BUCKET_MALLOC_FREE(arr);

    return old_it_nbytes;
}

int set(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv, const item* new_it) {
    bucket_array_t* arr;
    bucket_array_t* new_arr = NULL;
    ssize_t idx;
    bool replacing;

retry:
    arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

    idx = search_index(arr, search_key, nkey, hv);
    replacing = (idx != -1);
    if (!replacing) {
        idx = arr->len;
    }

    size_t new_len = arr->len + !replacing;
    if (!new_arr || new_arr->len < new_len) {
        new_arr = realloc_bucket_array(bucket, new_arr, new_len);
    } else {
        new_arr->len = new_len;
    }

    memcpy(new_arr->entries, arr->entries, arr->len * sizeof(array_entry_t));
    new_arr->entries[idx] = (array_entry_t){.it = (intptr_t)new_it, .hv = hv};

    if (!CAS(&bucket->arr, &arr, new_arr)) {
        recl_unprotect_pop_mem();
        goto retry;
    }

    recl_unprotect_pop_mem();

    int old_it_nbytes = 0;
    if(replacing) {
        // no need to protect item since we're the only ones who can free it
        item* old_it = AE_ITEM(arr->entries[idx]);
        old_it_nbytes = old_it->nbytes;
        recl_retire_item(old_it);
    }

    ASSOC_BUCKET_MALLOC_FREE(arr);

    return old_it_nbytes;
}

#if defined(RECL_EBR) || defined(RECL_QSENSE)
item* get_ebr(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv) {
    bucket_array_t* arr = AALOAD(bucket->arr);

    for (size_t i = 0; i < arr->len; i++) {
        array_entry_t e = arr->entries[i];
        if (key_equals(e, search_key, nkey, hv)) {
            return AE_ITEM(e);
        }
    }

    return NULL;
}
#endif

#if defined(RECL_HP) || defined(RECL_QSENSE)
item* get_hp(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv) {
    bucket_array_t* arr;
    item* it;

retry:
    arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

    for (size_t i = 0; i < arr->len; i++) {
        array_entry_t e = arr->entries[i];
        if (key_equals(e, search_key, nkey, hv)) {
            it = AE_ITEM(e);
            recl_protect_push_item(it);
            recl_unprotect_pop_mem();
            if (ALOAD(bucket->arr) != arr) {
                recl_unprotect_pop_item();
                goto retry;
            }
            return it;
        }
    }

    recl_unprotect_pop_mem();
    return NULL;
}
#endif

#ifdef RECL_QSENSE
item* get_qsense(bucket_t* bucket, const char* search_key, const size_t nkey, const uint16_t hv) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_EBR: return get_ebr(bucket, search_key, nkey, hv); break;
        case QSENSE_HP: return get_hp(bucket, search_key, nkey, hv); break;
        default:
            fprintf(stderr, "Unknown QSense mode in get_qsense: %d\n", qsense_get_protection_mode());
            exit(EXIT_FAILURE);
    }
}
#endif

int clear(bucket_t* bucket, size_t* freed_bytes) {
    int freed_items = 0;
    size_t freed_memory = 0;
    bucket_array_t* arr = bucket->arr;

    while (!CAS(&bucket->arr, &arr, &empty_bucket_array)) {
        if (arr == &empty_bucket_array) {
            *freed_bytes = 0;
            return 0;
        }
    }

    for (size_t i = 0; i < arr->len; i++) {
        array_entry_t e = arr->entries[i];
        item* it = AE_ITEM(e);
        recl_retire_item(it);
        freed_memory += ITEM_ntotal(it);
        freed_items++;
    }

    ASSOC_BUCKET_MALLOC_FREE(arr);

    *freed_bytes = freed_memory;
    return freed_items;
}


void assoc_bucket_init(size_t arena_size) {
#if defined(ASSOC_BUCKET_ARENA_MALLOC) && !defined(ASSOC_BUCKET_ARENA_NO_PADDING) && !defined(ASSOC_BUCKET_ARENA_MALLOC_FORCE_ALIGN)
    void* test = malloc(L1_DCACHE_LINE_SIZE_DEFAULT);
    if (!test || ((uintptr_t)test % L1_DCACHE_LINE_SIZE_DEFAULT)) {
        fprintf(stderr, "Failed to allocate properly aligned memory for bucket arrays.\n"
                        "Please define ASSOC_BUCKET_ARENA_MALLOC_FORCE_ALIGN.\n"
                        "Alternatively, use unpadded buckey-arrays (ASSOC_BUCKET_ARENA_NO_PADDING), bump-pointer allocation (ASSOC_BUCKET_ARENA_PERBUCKET, ASSOC_BUCKET_ARENA_GLOBAL), or an alternative allocator which provides aligned allocations by default (jemalloc, mimalloc).\n");
        exit(EXIT_FAILURE);
    }
    free(test);
#endif

#ifndef ASSOC_BUCKET_ARENA_MALLOC
    if (arena_size == 0) {
        arena_size = 1 << ARENA_SIZE_POWER_DEFAULT;
    }

    arena_init(arena_size);
#endif
}

bucket_t* new_bucket(void) {
    bucket_t* bucket = malloc(sizeof(bucket_t));
    if (!bucket) {
        // myTODO
        fprintf(stderr, "Failed to allocate bucket struct.\n");
        exit(EXIT_FAILURE);
    }

    bucket->arr = &empty_bucket_array;
#ifndef ASSOC_BUCKET_ARENA_MALLOC
    bucket->arena = new_arena();
#endif

    return bucket;
}


extern __thread int gt_tid;
extern /* atomic */ curr_items_t* curr_items; 
#define t_curr_items (curr_items[gt_tid].value)

void migrate_expanded_items(bucket_t** old_hashtable, bucket_t** new_hashtable, const uint64_t old_hashpower, const uint64_t new_hashpower) {
    uint64_t old_hashsize = hashsize(old_hashpower);
    for (uint64_t i = 0; i < old_hashsize; ++i) {
        bucket_t* bucket = old_hashtable[i];
        bucket_array_t* arr = recl_protect_push_load_mem(bucket->arr, __ATOMIC_ACQUIRE);

        // Traverse items in bucket
        for (size_t j = 0; j < arr->len; ++j) {
            array_entry_t e = arr->entries[j];
            item* it = AE_ITEM(e);
            recl_protect_push_item(it);
            char* key = ITEM_key(it);
            uint8_t nkey = it->nkey;
            uint64_t hv = hash(key, nkey);
            uint64_t bucket_new_idx = hv & hashmask(new_hashpower);

            if (i != bucket_new_idx) {
                bucket_t* bucket_new = new_hashtable[bucket_new_idx];
                uint16_t hv_msb = HV_16MSB(hv);
                // delete first to avoid reclamation problems due to double linking
                if (del(bucket, key, nkey, hv_msb, false) && insert(bucket_new, it, hv_msb)) {
                    // if insert fails, it means the item was replaced
                    // in that case, we should free the old item since it has essentially been deleted
                    recl_retire_item(it);
                    ASTORE(t_curr_items, t_curr_items - 1);
                }

                if (settings.verbose > 1) {
                    printf("Replaced item %.*s, from bucket %lu to %lu\n",
                                nkey, key, i, bucket_new_idx);
                }
            }

            recl_unprotect_pop_item();
        }

        recl_unprotect_pop_mem();
    }
}


#ifndef ASSOC_BUCKET_ARENA_MALLOC
#ifndef ASSOC_BUCKET_ARENA_GLOBAL

/* Arena allocation implementation */

typedef struct arena {
    uint8_t* addr;
    size_t size;
    size_t used;
} arena_t;


static size_t arena_size;

static __thread arena_t* t_arena_new;


static void arena_init(size_t _arena_size) {
    arena_size = _arena_size;
}

static arena_t* new_arena(void) {
    arena_t* arena = malloc(sizeof(arena_t));
    if (!arena) {
        // myTODO
        fprintf(stderr, "Failed to allocate arena struct.\n");
        exit(EXIT_FAILURE);
    }

    arena->size = ALOAD(arena_size);
    arena->addr = malloc(arena->size);
    if (!arena->addr) {
        // myTODO
        fprintf(stderr, "Failed to allocate arena.\n");
        exit(EXIT_FAILURE);
    }

    // Align first arena allocation to next cache line boundary
    size_t offset = MOD_POW2((uintptr_t)arena->addr, L1_DCACHE_LINE_SIZE_DEFAULT);
    arena->used = (offset > 0) ? L1_DCACHE_LINE_SIZE_DEFAULT - offset : 0;

    return arena;
}

__attribute__((always_inline))
static inline void arena_meta_free(arena_t* arena) {
    recl_retire_mem(arena->addr);
    recl_retire_mem(arena);
}

__attribute__((always_inline))
static inline bool arena_is_empty(arena_t* arena) {
    return ALOAD(arena->used) >= arena->size;
}

static void arena_advance(bucket_t* bucket) {
    arena_t* arena = AALOAD(bucket->arena);

    if (!arena_is_empty(arena)) {
        return;
    }

    if (!t_arena_new) {
        t_arena_new = new_arena();
    }

    if (__atomic_compare_exchange_n(&bucket->arena, &arena, t_arena_new,
                                    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        arena_meta_free(arena);
        t_arena_new = NULL;
    }
}

static bucket_array_t* arena_alloc(bucket_t* bucket, size_t size) {
#ifndef ASSOC_BUCKET_ARENA_NO_PADDING
    if (__glibc_likely(size <= L1_DCACHE_LINE_SIZE_DEFAULT)) {
        // This branch should always be taken
        size = L1_DCACHE_LINE_SIZE_DEFAULT;
    } else {
        size = ALIGN_UP_POW2(size, L1_DCACHE_LINE_SIZE_DEFAULT);
    }
#endif

    // Increase new arenas' size if a request exceeds the current size
    size_t curr_arena_size;
    while (size > (curr_arena_size = ALOAD(arena_size))) {
        __atomic_compare_exchange_n(&arena_size, &curr_arena_size, curr_arena_size * 2,
                                    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

retry:;
    arena_t* arena = AALOAD(bucket->arena);
    size_t ret_pos = ATOMIC_FADD(arena->used, size);

    if (ret_pos + size > arena->size) {
        arena_advance(bucket);
        goto retry;
    }

    return (bucket_array_t*)&arena->addr[ret_pos];
}

#else

typedef struct arena {
    uint8_t* ptr;
    atomic_size_t used;
    size_t size;
    /* number of currently active arrays in hash table + 1 if curr or t_arena_new */
    uint16_t active_count;
} arena_t;


static size_t arena_size;

static arena_t* curr_arena;
static __thread arena_t* t_arena_new;


static void arena_init(size_t _arena_size) {
    arena_size = _arena_size;
    curr_arena = new_arena();
}

static arena_t* new_arena(void) {
    arena_t* arena = malloc(sizeof(arena_t));
    if (!arena) {
        fprintf(stderr, "Failed to allocate arena struct.\n");
        exit(EXIT_FAILURE);
    }

    arena->size = ALOAD(arena_size);
    arena->ptr = malloc(arena->size);
    arena->active_count = 1;
    if (!arena->ptr) {
        fprintf(stderr, "Failed to allocate arena.\n");
        exit(EXIT_FAILURE);
    }

    // Align first arena allocation to next cache line boundary
    size_t offset = MOD_POW2((uintptr_t)arena->ptr, L1_DCACHE_LINE_SIZE_DEFAULT);
    arena->used = (offset > 0) ? L1_DCACHE_LINE_SIZE_DEFAULT - offset : 0;

    return arena;
}

__attribute__((always_inline))
static inline void arena_meta_free(arena_t* arena, bool private) {
    recl_retire_mem(arena->ptr);
    recl_retire_mem(arena);
}

__attribute__((always_inline))
static inline bool is_arena_full(arena_t* arena) {
    return arena->used >= arena->size;
}

static void arena_advance(bucket_t* bucket) {
    arena_t* l_curr_arena = curr_arena;

    if (!is_arena_full(l_curr_arena)) {
        return;
    }

    if (!t_arena_new) {
        t_arena_new = new_arena();
    }

    if (__atomic_compare_exchange_n(&curr_arena, &l_curr_arena, t_arena_new,
                                    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        // "Free" old arena
        if (ATOMIC_DECF(l_curr_arena->active_count) == 0) {
            recl_retire_mem(l_curr_arena->ptr);
            recl_retire_mem(l_curr_arena);
        }
        t_arena_new = NULL;
    }
}

static bucket_array_t* arena_alloc(bucket_t* bucket, size_t size) {
#ifndef ASSOC_BUCKET_ARENA_NO_PADDING
    if (__glibc_likely(size <= L1_DCACHE_LINE_SIZE_DEFAULT)) {
        // This branch should always be taken
        size = L1_DCACHE_LINE_SIZE_DEFAULT;
    } else {
        size = ALIGN_UP_POW2(size, L1_DCACHE_LINE_SIZE_DEFAULT);
    }
#endif

    // Increase new arenas' size if a request exceeds the current size
    size_t curr_arena_size;
    while (size > (curr_arena_size = ALOAD(arena_size))) {
        __atomic_compare_exchange_n(&arena_size, &curr_arena_size, curr_arena_size * 2,
                                    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

retry_alloc:;
    arena_t* arena = curr_arena;

    uint16_t active_count = ATOMIC_FINC(arena->active_count);
    if (active_count == 0) {
        // array was seen with active_count = 0 by another thread and was retired
        goto retry_alloc;
    }

    size_t ret_pos = ATOMIC_FADD(arena->used, size);

    if (ret_pos + size > arena->size) {
        ATOMIC_DEC(arena->active_count);
        arena_advance(bucket);
        goto retry_alloc;
    }

    bucket_array_t* ret = (bucket_array_t*)&arena->ptr[ret_pos];
    ret->arena = arena;

    return ret;
}

static inline void arena_free(bucket_array_t* arr) {
    arena_t* arena = arr->arena;
    if (ATOMIC_DECF(arena->active_count) == 0) {
        recl_retire_mem(arena->ptr);
        recl_retire_mem(arena);
    }
}

#endif
#endif

#else

__attribute__((unused)) static const char assoc_bucket_empty_file_warning;

#endif