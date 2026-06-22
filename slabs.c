#if !defined(SLAB_PER_CLASS_LOCKING) && !defined(SLAB_GLOBAL_LOCK)

#include "memcached.h"

extern size_t assoc_clock_frequency;

enum {
    MEM_PRESSURE_NONE = 16,
    MEM_PRESSURE_LOW = 8,
    MEM_PRESSURE_MEDIUM = 4,
    MEM_PRESSURE_HIGH = 2,
    MEM_PRESSURE_EXTREME = 1
} mem_pressure_state = MEM_PRESSURE_NONE;

// Memory pressure thresholds for adjusting assoc clock frequency
#define MEM_PRESSURE_LOW_INTERVAL       (1 << 31)   // 2GB
#define MEM_PRESSURE_MEDIUM_INTERVAL    (1 << 29)   // 512MB
#define MEM_PRESSURE_HIGH_INTERVAL      (1 << 27)   // 128MB
#define MEM_PRESSURE_EXTREME_INTERVAL   (1 << 20)   // 1MB

static size_t MEM_PRESSURE_LOW_THRESHOLD;
static size_t MEM_PRESSURE_MEDIUM_THRESHOLD;
static size_t MEM_PRESSURE_HIGH_THRESHOLD;
static size_t MEM_PRESSURE_EXTREME_THRESHOLD;

#ifdef RECL_QSENSE
static size_t MEM_PRESSURE_QSENSE_EBR_THRESHOLD;
#endif

static size_t mem_limit;
static size_t mem_used = 0;

static void mem_pressure_monitoring_init(size_t limit) {
    mem_limit = limit;

    MEM_PRESSURE_LOW_THRESHOLD = (MEM_PRESSURE_LOW_INTERVAL > mem_limit) ? 0 :
        mem_limit - MEM_PRESSURE_LOW_INTERVAL;
    MEM_PRESSURE_MEDIUM_THRESHOLD = (MEM_PRESSURE_MEDIUM_INTERVAL > mem_limit) ? 0 :
        mem_limit - MEM_PRESSURE_MEDIUM_INTERVAL;
    MEM_PRESSURE_HIGH_THRESHOLD = (MEM_PRESSURE_HIGH_INTERVAL > mem_limit) ? 0 :
        mem_limit - MEM_PRESSURE_HIGH_INTERVAL;
    MEM_PRESSURE_EXTREME_THRESHOLD = (MEM_PRESSURE_EXTREME_INTERVAL > mem_limit) ? 0 :
        mem_limit - MEM_PRESSURE_EXTREME_INTERVAL;
#ifdef RECL_QSENSE
    MEM_PRESSURE_QSENSE_EBR_THRESHOLD = (QSENSE_EBR_SWITCH_MEM_THRESHOLD > mem_limit) ? 0 :
        mem_limit - QSENSE_EBR_SWITCH_MEM_THRESHOLD;
#endif

}

static void adjust_mem_pressure(size_t new_mem_used) {
    switch (ALOAD(assoc_clock_frequency)) {
        case MEM_PRESSURE_NONE:
            if (new_mem_used >= MEM_PRESSURE_LOW_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_LOW);
            }
            break;
        case MEM_PRESSURE_LOW:
            if (new_mem_used >= MEM_PRESSURE_MEDIUM_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_MEDIUM);
            } else if (new_mem_used < MEM_PRESSURE_LOW_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_NONE);
            }
            break;
        case MEM_PRESSURE_MEDIUM:
            if (new_mem_used >= MEM_PRESSURE_HIGH_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_HIGH);
            } else if (new_mem_used < MEM_PRESSURE_MEDIUM_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_LOW);
            }
            break;
        case MEM_PRESSURE_HIGH:
            if (new_mem_used >= MEM_PRESSURE_EXTREME_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_EXTREME);
                recl_advise_mem_pressure();
            } else if (new_mem_used < MEM_PRESSURE_HIGH_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_MEDIUM);
            }
#ifdef RECL_QSENSE
            if (new_mem_used > MEM_PRESSURE_QSENSE_EBR_THRESHOLD) {
                recl_advise_normal_operation();
            }
#endif
            break;
        case MEM_PRESSURE_EXTREME:
            if (new_mem_used < MEM_PRESSURE_EXTREME_THRESHOLD) {
                ASTORE(assoc_clock_frequency, MEM_PRESSURE_HIGH);
            }
            break;
        default:
            ASTORE(assoc_clock_frequency, MEM_PRESSURE_NONE);
            break;
    }
}

#ifdef USE_SLAB_ALLOCATOR

/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/cdefs.h>

//#define DEBUG_SLAB_MOVER
/* powers-of-N allocation structures */

typedef struct slab {
    struct slab* next; /* next slab in list */
    void *addr;                 /* slab's address */
    void* slots;       /* list of item ptrs */
} slab_t;

typedef struct {
    uint32_t size;              /* sizes of items */
    uint32_t perslab;           /* how many items per slab */
    slab_t* slab_list; /* head of slab list */
} slabclass_t;


static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
#ifndef SLAB_NO_CURSOR
/* last available slab, excluding global */
static __thread slab_t *t_slab_cursor[MAX_NUMBER_OF_SLAB_CLASSES - 1];
#endif

static size_t slabs_mem_limit = 0;
static int power_largest;

static void *mem_base = NULL;
static void * mem_current = NULL;
static size_t mem_avail = 0;

/*
 * Forward Declarations
 */
static slab_t *do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static slab_t *memory_allocate_slab(size_t size);

/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate(const unsigned int maxslabs);

/* Find a non-empty slab in the specified slab class. */
static slab_t *find_available_slab(unsigned int id, bool alloc_new);

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */
#ifdef ALLOCATION_CLASS_DISTRIBUTION
#include <math.h>

__thread uint32_t t_clsid_seed;
static unsigned int clsid_power_largest;

unsigned int slabs_clsid(const size_t size) {
    if (size == 0 || size > slabclass[POWER_SMALLEST].size) {
        return 0;
    }

    return (t_clsid_seed++ % clsid_power_largest) + 1;
}
#else
unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0 || size > settings.item_size_max)
        return 0;
    while (size > slabclass[res].size)
        if (res++ == power_largest)     /* won't fit in the biggest slab */
            return power_largest;
    return res;
}
#endif

unsigned int slabs_size(const int clsid) {
    return slabclass[clsid].size;
}

// TODO: could this work with the restartable memory?
// Docs say hugepages only work with private shm allocs.
/* Function split out for better error path handling */
static void * alloc_large_chunk(const size_t limit)
{
    void *ptr = NULL;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    size_t pagesize = 0;
    FILE *fp;
    int ret;

    /* Get the size of huge pages */
    fp = fopen("/proc/meminfo", "r");
    if (fp != NULL) {
        char buf[64];

        while ((fgets(buf, sizeof(buf), fp)))
            if (!strncmp(buf, "Hugepagesize:", 13)) {
                ret = sscanf(buf + 13, "%zu\n", &pagesize);

                /* meminfo huge page size is in KiBs */
                pagesize <<= 10;
            }
        fclose(fp);
    }

    if (!pagesize) {
        fprintf(stderr, "Failed to get supported huge page size\n");
        return NULL;
    }

    if (settings.verbose > 1)
        fprintf(stderr, "huge page size: %zu\n", pagesize);

    /* This works because glibc simply uses mmap when the alignment is
     * above a certain limit. */
    ret = posix_memalign(&ptr, pagesize, limit);
    if (ret != 0) {
        fprintf(stderr, "Failed to get aligned memory chunk: %d\n", ret);
        return NULL;
    }

    ret = madvise(ptr, limit, MADV_HUGEPAGE);
    if (ret < 0) {
        fprintf(stderr, "Failed to set transparent hugepage hint: %d\n", ret);
        free(ptr);
        ptr = NULL;
    }
#elif defined(__FreeBSD__)
    size_t align = (sizeof(size_t) * 8 - (__builtin_clzl(4095)));
    ptr = mmap(NULL, limit, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | MAP_ALIGNED(align) | MAP_ALIGNED_SUPER, -1, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "Failed to set super pages\n");
        ptr = NULL;
    }
#else
    ptr = malloc(limit);
#endif
    return ptr;
}

unsigned int slabs_fixup(char *chunk, const int border) {
    fprintf(stderr, "Restart not yet implemented.\n");
    exit(EXIT_FAILURE);

//!!    slabclass_t *p;
//!!    item *it = (item *)chunk;
//!!    int id = ITEM_clsid(it);
//!!
//!!    // memory isn't used yet. shunt to global pool.
//!!    // (which must be 0)
//!!    if (id == 0) {
//!!        //assert(border == 0);
//!!        p = &slabclass[0];
//!!        do_grow_slab_list(0);
//!!        p->slab_list[p->num_slabs++] = (char*)chunk;
//!!        return -1;
//!!    }
//!!    p = &slabclass[id];
//!!
//!!    // if we're on a page border, add the slab to slab class
//!!    if (border == 0) {
//!!        do_grow_slab_list(id);
//!!        p->slab_list[p->num_slabs++] = chunk;
//!!    }
//!!
//!!    // increase free count if ITEM_SLABBED
//!!    if (it->it_flags == ITEM_SLABBED) {
//!!        // if ITEM_SLABBED re-stack on freelist.
//!!        // don't have to run pointer fixups.
//!!        it->prev = 0;
//!!        it->next = p->slots;
//!!        if (it->next) it->next->prev = it;
//!!        p->slots = it;
//!!
//!!        p->sl_curr++;
//!!        //fprintf(stderr, "replacing into freelist\n");
//!!    }
//!!
//!!    return p->size;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, void *mem_base_external, bool reuse_mem) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;

    /* Some platforms use runtime transparent hugepages. If for any reason
     * the initial allocation fails, the required settings do not persist
     * for remaining allocations. As such it makes little sense to do slab
     * preallocation. */
    bool __attribute__ ((unused)) do_slab_prealloc = false;

    slabs_mem_limit = limit;

    if (prealloc && mem_base_external == NULL) {
        mem_base = alloc_large_chunk(slabs_mem_limit);
        if (mem_base) {
            do_slab_prealloc = true;
            mem_current = mem_base;
            mem_avail = slabs_mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
    } else if (prealloc && mem_base_external != NULL) {
        // Can't (yet) mix hugepages with mmap allocations, so separate the
        // logic from above. Reusable memory also force-preallocates memory
        // pages into the global pool, which requires turning mem_* variables.
        do_slab_prealloc = true;
        mem_base = mem_base_external;
        // _current shouldn't be used in this case, but we set it to where it
        // should be anyway.
        if (reuse_mem) {
            mem_current = ((char*)mem_base) + slabs_mem_limit;
            mem_avail = 0;
        } else {
            mem_current = mem_base;
            mem_avail = slabs_mem_limit;
        }
    }

    memset(slabclass, 0, sizeof(slabclass));

    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
        if (slab_sizes != NULL) {
            if (slab_sizes[i-1] == 0)
                break;
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor) {
            break;
        }
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        slabclass[i].size = size;
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        if (slab_sizes == NULL)
            size *= factor;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }

    power_largest = i;
#ifdef ALLOCATION_CLASS_DISTRIBUTION
    slabclass[power_largest].size = slabclass[power_largest - 1].size;
    slabclass[power_largest].perslab = slabclass[power_largest - 1].perslab;
#else
    slabclass[power_largest].size = settings.slab_chunk_size_max;
    slabclass[power_largest].perslab = settings.slab_page_size / settings.slab_chunk_size_max;
#endif
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            int64_t env_malloced;
            if (safe_strtoll((const char *)t_initial_malloc, &env_malloced)) {
                mem_used = (size_t)env_malloced;
            }
        }

    }

    if (do_slab_prealloc) {
        if (!reuse_mem) {
            slabs_preallocate(power_largest);
        }
    }

#ifdef ALLOCATION_CLASS_DISTRIBUTION
    clsid_power_largest = settings.allocation_class_distribution_nclasses;
#endif

    mem_pressure_monitoring_init(limit);
}

void slabs_prefill_global(void) {
    slabclass_t *p = &slabclass[0];
    slab_t **slab_ptr = (slab_t **)&p->slab_list;
    slab_t *new_slab;
    int len = settings.slab_page_size;

    while (mem_used < slabs_mem_limit
            && (new_slab = memory_allocate_slab(len)) != NULL) {
        // Ensure the front header is zero'd to avoid confusing restart code.
        // It's probably good enough to cast it and just zero slabs_clsid, but
        // this is extra paranoid.
        //memset(new_slab->addr, 0, sizeof(item));
        *slab_ptr = new_slab;
        slab_ptr = (slab_t **)&((*slab_ptr)->next);
    }
}

static void slabs_preallocate(const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs)
            break;
        if (do_slabs_newslab(i) == NULL) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max memory must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }
}

static void split_slab_page_into_freelist(slab_t *s, unsigned int id) {
    slabclass_t *p = &slabclass[id];
    char *ptr = (char *)s->addr;

    assert(s->slots == NULL);

    for (int i = 0; i < p->perslab; i++) {
        item *it = (item *)ptr;
        it->it_flags = ITEM_SLABBED;
        it->slab = s;
        it->next = s->slots;
        s->slots = it;

        ptr += p->size;
    }
}

static slab_t *get_slab_from_global_pool(void) {
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    slab_t *s = g->slab_list;

    do {
        if (s == NULL) {
            return NULL;
        }
    } while (!__atomic_compare_exchange_n(&g->slab_list, &s, s->next, \
        true, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));

    return s;
}

static slab_t* do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    slab_t **slab_ptr;
    slab_t *new_slab, *next_slab;
    int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
    ? settings.slab_page_size
    : p->size * p->perslab;

    if (slabs_mem_limit && mem_used + len > slabs_mem_limit && p->slab_list != NULL
        && g->slab_list == NULL) {
            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return NULL;
        }

        if (((new_slab = get_slab_from_global_pool()) == NULL) &&
            ((new_slab = memory_allocate_slab((size_t)len)) == NULL)) {

            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return NULL;
        }

    // Not zeroing item headers here. only it_flags should is assumed to not have junk, but it should be fine as it currently is.
    split_slab_page_into_freelist(new_slab, id);

    slab_ptr = &p->slab_list;
    do {
        // Find slab list tail.
        while (*slab_ptr != NULL) {
            slab_ptr = &(AALOAD(*slab_ptr)->next);
        }
        next_slab = NULL;
    } while (!__atomic_compare_exchange_n(slab_ptr, &next_slab, new_slab, \
            false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);
    return new_slab;
}

#ifdef SLAB_NO_CURSOR
static slab_t *find_available_slab(unsigned int id, bool alloc_new) {
    slab_t *s = slabclass[id].slab_list;

    while (s != NULL && AALOAD(s)->slots == NULL) {
        s = s->next;
    }

    if (s == NULL && alloc_new) {
        s = do_slabs_newslab(id);
    }

    return s;
}
#else
static slab_t *find_available_slab(unsigned int id, bool alloc_new) {
    slabclass_t *p = &slabclass[id];
    slab_t **cursor = &t_slab_cursor[id - 1];

    // search until end of list, starting from last available slab
    for (slab_t *s = *cursor ? *cursor : p->slab_list; s != NULL; s = s->next) {
        if (AALOAD(s)->slots != NULL) {
            *cursor = s;
            return s;
        }
    }

    // if cursor was used, wrap around and search remaining slabs
    if (*cursor != NULL) {
        // must also check for null in case start was removed in the meantime
        for (slab_t *s = p->slab_list; s != *cursor && s != NULL; s = s->next) {
            if (AALOAD(s)->slots != NULL) {
                *cursor = s;
                return s;
            }
        }
    }

    *cursor = alloc_new ? do_slabs_newslab(id) : NULL;
    return *cursor;
}
#endif

static void *do_slabs_alloc(unsigned int id,
        unsigned int flags) {
    slab_t *s;
    void *ret = NULL;
    item *it = NULL;
    bool alloc_new = flags != SLABS_ALLOC_NO_NEWPAGE;

    if (id < POWER_SMALLEST || id > power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(id);
        return NULL;
    }

    s = find_available_slab(id, alloc_new);

    if (s != NULL) {
        assert(((item *)AALOAD(s->slots))->it_flags & ITEM_SLABBED);
        /* return off the slab's freelist */
        it = (item *)AALOAD(s->slots);
        do {
            while (it == NULL) {
                /* slab became empty, find another one unless we're out of memory */
                if ((s = find_available_slab(id, alloc_new)) == NULL) {
                    ret = NULL;
                    goto _do_slabs_alloc_done;
                }
                it = (item *)AALOAD(s->slots);
            }
        } while (!__atomic_compare_exchange_n(&s->slots, &it, it->next, \
                true, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

        it->it_flags &= ~ITEM_SLABBED;
        ret = (void *)it;
    } else {
        ret = NULL;
    }

_do_slabs_alloc_done:
    if (ret) {
        MEMCACHED_SLABS_ALLOCATE(id, slabclass[id].size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(id);
    }

    return ret;
}

static void do_slabs_free_chunked(item *it) {
    item_chunk *chunk = (item_chunk *) ITEM_schunk(it);
    slab_t* s = it->slab;

    it->it_flags = ITEM_SLABBED;

    if (chunk->next) {
        chunk = chunk->next;
        chunk->prev = 0;
    } else {
        // header with no attached chunk
        chunk = NULL;
    }

    // return the header object.
    // TODO: This is in three places, here and in do_slabs_free().
    it->next = s->slots;
    while (!__atomic_compare_exchange_n(&s->slots, &it->next, it, \
            true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}

    while (chunk) {
        assert(chunk->it_flags == ITEM_CHUNK);
        chunk->it_flags = ITEM_SLABBED;
        s = chunk->slab;
        item_chunk *next_chunk = chunk->next;

        chunk->prev = 0;
        chunk->next = s->slots;
        while (!__atomic_compare_exchange_n(&s->slots, &chunk->next, chunk, \
                true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}

        chunk = next_chunk;
    }

    return;
}

static void do_slabs_free(void *ptr) {
    slab_t *s;
    item *it;

    it = (item *)AALOAD(ptr);
    s = it->slab;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        it->it_flags = ITEM_SLABBED;
        it->next = s->slots;
        while (!__atomic_compare_exchange_n(&s->slots, &it->next, it, \
                true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    } else {
        do_slabs_free_chunked(it);
    }
    return;
}

/* With refactoring of the various stats code the automover won't need a
 * custom function here.
 */
void fill_slab_stats_automove(slab_stats_automove *am) {
    int n;
    //!! pthread_mutex_lock(&slabs_lock);
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        slabclass_t *p = &slabclass[n];
        slab_stats_automove *cur = &am[n];
        cur->chunks_per_page = p->perslab;
        //!! cur->free_chunks = p->sl_curr;
        //!! cur->total_pages = p->num_slabs;
        cur->chunk_size = p->size;
    }
    //!! pthread_mutex_unlock(&slabs_lock);
}

bool slabs_out_of_memory(void) {
    return mem_used >= slabs_mem_limit &&
        slabclass[SLAB_GLOBAL_PAGE_POOL].slab_list == NULL;
}

unsigned int global_page_pool_size(void) {
    unsigned int ret = 0;
    //!! ret = slabclass[SLAB_GLOBAL_PAGE_POOL].slabs;
    return ret;
}

/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;
    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats, NULL);

    total = 0;
    for(i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];
        if (p->slab_list != NULL) {
            uint32_t perslab; //!! , slabs;
            //!! slabs = p->num_slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            //!! APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            //!! APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            //!! APPEND_NUM_STAT(i, "used_chunks", "%u",
            //!!                 slabs*perslab - p->sl_curr);
            //!! APPEND_NUM_STAT(i, "free_chunks", "%u", p->sl_curr);
            /* Stat is dead, but displaying zero instead of removing it. */
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", 0);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)mem_used);
    add_stats(NULL, 0, NULL, 0, c);
}

static slab_t *memory_allocate_slab(size_t size) {
    void *addr = memory_allocate(size);
    if (addr == NULL) {
        return NULL;
    }

    slab_t *s = calloc(1, sizeof(slab_t));
    s->addr = addr;
    return s;
}

static void *memory_allocate(size_t size) {
    void *ret;

    if (mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */
        ret = malloc(size);
    } else {
        ret = mem_current;

        if (size > mem_avail) {
            return NULL;
        }

        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }
    mem_used += size;

    adjust_mem_pressure(mem_used);
    return ret;
}

/* Must only be used if all pages are item_size_max */
static void memory_release(void) {
    slab_t *s = NULL;
    if (mem_base != NULL)
        return;

    if (!settings.slab_reassign)
        return;

    while (mem_used > slabs_mem_limit &&
            (s = get_slab_from_global_pool()) != NULL) {
        free(s->addr);
        free(s);
        mem_used -= settings.slab_page_size;
    }
}

void *slabs_alloc(unsigned int id, unsigned int flags) {
    return do_slabs_alloc(id, flags);
}

void slabs_free(void *ptr) {
    do_slabs_free(ptr);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    do_slabs_stats(add_stats, c);
}

static bool do_slabs_adjust_mem_limit(size_t new_mem_limit) {
    /* Cannot adjust memory limit at runtime if prealloc'ed */
    if (mem_base != NULL)
        return false;
    settings.maxbytes = new_mem_limit;
    slabs_mem_limit = new_mem_limit;
    memory_release(); /* free what might already be in the global pool */
    return true;
}

bool slabs_adjust_mem_limit(size_t new_mem_limit) {
    return do_slabs_adjust_mem_limit(new_mem_limit);
}

void *slabs_peek_page(const unsigned int id, uint32_t *size, uint32_t *perslab) {
    slabclass_t *s_cls;
    void *page = NULL;
    if (id > power_largest) {
        return NULL;
    }

    s_cls = &slabclass[id];
    //!! if (s_cls->slabs < 2) {
    //!!     return NULL;
    //!! }
    *size = s_cls->size;
    *perslab = s_cls->perslab;

    //!! page = s_cls->slab_list[0];

    return page;
}

/* detaches item/chunk from freelist.
 * for use with page mover.
 * lock _must_ be held.
 */
void do_slabs_unlink_free_chunk(const unsigned int id, item *it) {
    //!! slabclass_t *s_cls = &slabclass[id];
    //!! /* Ensure this was on the freelist and nothing else. */
    //!! assert(it->it_flags == ITEM_SLABBED);
    //!! if (s_cls->slots == it) {
    //!!     s_cls->slots = it->next;
    //!! }
    //!! if (it->next) it->next->prev = it->prev;
    //!! if (it->prev) it->prev->next = it->next;
    //!! s_cls->sl_curr--;
}

void slabs_finalize_page_move(const unsigned int sid, const unsigned int did, void *page) {
    //!! pthread_mutex_lock(&slabs_lock);
    //!! slabclass_t *s_cls = &slabclass[sid];
    //!! slabclass_t *d_cls = &slabclass[did];

    //!! s_cls->num_slabs--;
    //!! for (int x = 0; x < s_cls->num_slabs; x++) {
    //!!     s_cls->slab_list[x] = s_cls->slab_list[x+1];
    //!! }

    // FIXME: it's nearly impossible for this to fail, and error handling here
    // is gnarly since we'll have to just put the page back where we got it
    // from.
    // For now we won't handle the error, and a subsequent commit should
    // remove the need to resize the slab list.
    //!! do_grow_slab_list(did);
    //!! d_cls->slab_list[d_cls->num_slabs++] = page;
    /* Don't need to split the page into chunks if we're just storing it */
    if (did > SLAB_GLOBAL_PAGE_POOL) {
        memset(page, 0, (size_t)settings.slab_page_size);
        split_slab_page_into_freelist(page, did);
    } else if (did == SLAB_GLOBAL_PAGE_POOL) {
        /* memset just enough to signal restart handler to skip */
        memset(page, 0, sizeof(item));
        /* mem_malloc'ed might be higher than slabs_mem_limit. */
        memory_release();
    }

    //!! pthread_mutex_unlock(&slabs_lock);
}
/* Iterate at most once through the slab classes and pick a "random" source.
 * I like this better than calling rand() since rand() is slow enough that we
 * can just check all of the classes once instead.
 */
int slabs_pick_any_for_reassign(const unsigned int did) {
    //!! pthread_mutex_lock(&slabs_lock);
    static int cur = POWER_SMALLEST - 1;
    int tries = MAX_NUMBER_OF_SLAB_CLASSES - POWER_SMALLEST + 1;
    for (; tries > 0; tries--) {
        cur++;
        if (cur > MAX_NUMBER_OF_SLAB_CLASSES)
            cur = POWER_SMALLEST;
        if (cur == did)
            continue;
        //!! if (slabclass[cur].slabs > 1) {
        //!!     pthread_mutex_unlock(&slabs_lock);
        //!!     return cur;
        //!! }
    }
    //!! pthread_mutex_unlock(&slabs_lock);
    return -1;
}

int slabs_page_count(const unsigned int id) {
    int ret;
    ret = 0; //!! slabclass[id].slabs;
    return ret;
}

#else

void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, void *mem_base_external, bool reuse_mem) {
    mem_pressure_monitoring_init(limit);
}

#ifndef NO_MEM_LIMIT

void *slabs_alloc(size_t size) {
    size_t new_mem_used;
    size_t l_mem_used = ALOAD(mem_used);

    // Check and update memory usage
    do {
        new_mem_used = l_mem_used + size;
        if (new_mem_used > mem_limit) {
            return NULL;
        }
    } while (!__atomic_compare_exchange_n(&mem_used, &l_mem_used, new_mem_used, \
            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

#ifndef NO_MEM_PRESSURE_MONITORING
    adjust_mem_pressure(new_mem_used);
#endif

    return malloc(size);
}

void slabs_free(void* ptr) {
    item* it = (item *)AALOAD(ptr);
    ATOMIC_SUB(mem_used, ITEM_ntotal(it));
    free(ptr);
}

void slabs_free_batch_finish(size_t bytes_freed) {
    ATOMIC_SUB(mem_used, bytes_freed);
}

size_t get_mem_free(void) {
    return mem_limit - ALOAD(mem_used);
}
#endif

#endif

#else

__attribute__((unused)) static const int a;

#endif
