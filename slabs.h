/* slabs memory allocation */
#ifndef SLABS_H
#define SLABS_H

#ifdef USE_SLAB_ALLOCATOR

struct slab;

/** Init the subsystem. 1st argument is the limit on no. of bytes to allocate,
    0 if no limit. 2nd argument is the growth factor; each slab will use a chunk
    size equal to the previous slab's chunk size times this factor.
    3rd argument specifies if the slab allocator should allocate all memory
    up front (if true), or allocate memory in chunks as it is needed (if false)
*/
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, void *mem_base_external, bool reuse_mem);

/** Call only during init. Pre-allocates all available memory */
void slabs_prefill_global(void);

/**
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size);
unsigned int slabs_size(const int clsid);

/** Allocate object of given length. 0 on error */ /*@null@*/
#define SLABS_ALLOC_NO_NEWPAGE 1
void *slabs_alloc(unsigned int id, unsigned int flags);

/** Free previously allocated object */
void slabs_free(void *ptr);

/** Adjust global memory limit up or down */
bool slabs_adjust_mem_limit(size_t new_mem_limit);

typedef struct {
    unsigned int chunks_per_page;
    unsigned int chunk_size;
    long int free_chunks;
    long int total_pages;
} slab_stats_automove;
void fill_slab_stats_automove(slab_stats_automove *am);
bool slabs_out_of_memory(void);
unsigned int global_page_pool_size(void);

/** Fill buffer with stats */ /*@null@*/
void slabs_stats(ADD_STAT add_stats, void *c);

/* utilities for page moving */
void *slabs_peek_page(const unsigned int id, uint32_t *size, uint32_t *perslab);
void do_slabs_unlink_free_chunk(const unsigned int id, item *it);
void slabs_finalize_page_move(const unsigned int sid, const unsigned int did, void *page);
int slabs_pick_any_for_reassign(const unsigned int did);
int slabs_page_count(const unsigned int id);

/* Fixup for restartable code. */
unsigned int slabs_fixup(char *chunk, const int border);

#define slabs_free_batch_incremental(ptr, bytes_freed) slabs_free(ptr)
#define slabs_free_batch_finish(bytes_freed) do { } while (0)
#define get_mem_free() (0)

#else

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "memcached.h"

void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, void *mem_base_external, bool reuse_mem);
#ifdef NO_MEM_LIMIT
#define slabs_alloc(size) malloc(size)
#define slabs_free(ptr) free(ptr)
#else
void* slabs_alloc(size_t size);
void slabs_free(void *ptr);
#endif
#define slabs_prefill_global() do { } while (0)
#define slabs_clsid(size) (0)
#define slabs_size(clsid) (0)
#define slabs_adjust_mem_limit(new_mem_limit) (false)
#define slabs_out_of_memory() (true)
#define global_page_pool_size() (0)
#define slabs_stats(add_stats, c) do { } while (0)
#define slabs_peek_page(id, size, perslab) (NULL)
#define do_slabs_unlink_free_chunk(id, it) do { } while (0)
#define slabs_finalize_page_move(sid, did, page) do { } while (0)
#define slabs_pick_any_for_reassign(did) (-1)
#define slabs_page_count(id) (0)
#define slabs_fixup(chunk, border) (0)

#ifdef NO_MEM_LIMIT
#define slabs_free_batch_incremental(ptr, bytes_freed) free(ptr)
#define slabs_free_batch_finish(bytes_freed) do { } while (0)
#define get_mem_free() (0)
#else
// Used to avoid high contention overhead on mem_used shared variable
__attribute__((always_inline))
static inline void slabs_free_batch_incremental(void* ptr, size_t* bytes_freed) {
    size_t size = ITEM_ntotal((item*)ptr);
    free(ptr);
    *bytes_freed += size;
}

void slabs_free_batch_finish(size_t bytes_freed);
size_t get_mem_free(void);
#endif

#endif // USE_SLAB_ALLOCATOR

#endif
