/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <stdlib.h>
#include "memcached.h"
#include "slabs_mover.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>
#include <stdatomic.h>


static uint64_t cas_id = 0;

/* Get the next CAS id for a new item. */
uint64_t get_cas_id(void) {
    return ATOMIC_INCF(cas_id);
}

void set_cas_id(uint64_t new_cas) {
    ASTORE(cas_id, new_cas);
}

int item_is_flushed(item *it) {
    rel_time_t oldest_live = settings.oldest_live;
    uint64_t cas = ITEM_get_cas(it);
    uint64_t oldest_cas = settings.oldest_cas;
    if (oldest_live == 0 || oldest_live > current_time)
        return 0;
    if ((it->time <= oldest_live)
            || (oldest_cas != 0 && cas != 0 && cas < oldest_cas)) {
        return 1;
    }
    return 0;
}

/**
 * Generates the variable-sized part of the header for an object.
 *
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    if (flags == 0) {
        *nsuffix = 0;
    } else {
        *nsuffix = sizeof(flags);
    }
    return sizeof(item) + nkey + *nsuffix + nbytes;
}

#ifdef USE_SLAB_ALLOCATOR
#define do_item_alloc_pull_slabs_alloc() slabs_alloc(id, 0)
#else
#define do_item_alloc_pull_slabs_alloc() slabs_alloc(ntotal)
#endif

#ifdef RECL_QSENSE
// For simplicity, just use the default values with qsense, ignore user-provided ones.
#define oom_recl_ntries_ebr OOM_RECL_NTRIES_EBR
#define oom_recl_usleep_ebr OOM_RECL_USLEEP_EBR
#define oom_recl_ntries_hp OOM_RECL_NTRIES_HP
#define oom_recl_usleep_hp OOM_RECL_USLEEP_HP
#else
#define oom_recl_ntries_ebr settings.oom_recl_ntries
#define oom_recl_usleep_ebr settings.oom_recl_usleep
#define oom_recl_ntries_hp settings.oom_recl_ntries
#define oom_recl_usleep_hp settings.oom_recl_usleep
#endif

#if defined(RECL_EBR) || defined(RECL_QSENSE)
static item* force_reclaim_item_alloc_ebr(const size_t ntotal, const unsigned int id, bool* given_up) {
    size_t total_bags_freed = 0;
    *given_up = false;

    for (int i = 0; i < oom_recl_ntries_ebr; i++) {
        size_t bags_freed = ebr_force_announce_epoch();
        if (bags_freed > 0) {
            total_bags_freed += bags_freed;

            item* it = do_item_alloc_pull_slabs_alloc();
            if (it != NULL)
                return it;

            // Advance at least two epochs before "giving up" so that any evicted items are reclaimed
            if (total_bags_freed < NUM_LIMBO_BAGS) {
                continue;
            }
            
            // It's possible that other threads haven't yet seen the announced epoch and/or finished reclaiming their retired memory
            // Wait a bit and try again
            usleep(oom_recl_usleep_ebr);
            return do_item_alloc_pull_slabs_alloc();
        }

        usleep(oom_recl_usleep_ebr);
    }

    // Couldn't advance epoch
    *given_up = true;
    return do_item_alloc_pull_slabs_alloc();
}
#endif

#if defined(RECL_HP) || defined(RECL_QSENSE)
static item* force_reclaim_item_alloc_hp(const size_t ntotal, const unsigned int id, bool* given_up) {
    size_t mem_freed = hp_do_system_item_reclamation();
    *given_up = false;

    for (int i = 0; i < oom_recl_ntries_hp; i++) {
        if (mem_freed >= ntotal || t_recl_hp->retired_items.size == 0) {
            item* it = do_item_alloc_pull_slabs_alloc();
            if (it != NULL)
                return it;

            usleep(oom_recl_usleep_hp);
            return do_item_alloc_pull_slabs_alloc();
        }

        usleep(oom_recl_usleep_hp);

        if (t_recl_hp->retired_items.size > 0)
            mem_freed += hp_reclaim_items();
    }

    // Couldn't advance epoch
    *given_up = true;
    return do_item_alloc_pull_slabs_alloc();
}
#endif

#if defined(RECL_QSENSE)
static item* force_reclaim_item_alloc_qsense(const size_t ntotal, const unsigned int id, bool* given_up) {
    qsense_mode_e mode = qsense_get_protection_mode();
    switch(mode) {
        case QSENSE_EBR: return force_reclaim_item_alloc_ebr(ntotal, id, given_up); break;
        case QSENSE_HP: return force_reclaim_item_alloc_hp(ntotal, id, given_up); break;
        default:
            fprintf(stderr, "Unknown QSense mode in force_reclaim_item_alloc_qsense: %d\n", mode);
            exit(EXIT_FAILURE);
    }
}
#endif

#ifdef RECL_EBR
#define force_reclaim_item_alloc force_reclaim_item_alloc_ebr
#elif defined(RECL_HP)
#define force_reclaim_item_alloc force_reclaim_item_alloc_hp
#elif defined(RECL_QSENSE)
#define force_reclaim_item_alloc force_reclaim_item_alloc_qsense
#else
#error "No reclamation scheme defined. Define RECL_EBR, RECL_HP, or RECL_QSENSE."
#endif

static void do_item_evict(const size_t ntotal) {
    size_t total_mem_freed = 0;
    int nitems = 0;

    do {
        size_t mem_freed;
        recl_leave_quiescent();
        nitems += try_evict(ntotal, &mem_freed);
        recl_enter_quiescent();
        total_mem_freed += mem_freed;
    // Always evict some memory
    // Otherwise, if force is set, evict until we have enough
    } while (total_mem_freed < ntotal);

    t_stats_state.curr_items -= nitems;
    t_stats_state.curr_bytes -= total_mem_freed;
}

item *do_item_alloc_pull(const size_t ntotal, const unsigned int id) {
#ifdef USE_SLAB_ALLOCATOR
    if (id == 0)
        return NULL;
#endif

    item *it = do_item_alloc_pull_slabs_alloc();
    if(it != NULL)
        return it;

    if (recl_is_state_locked()) {
        // Some other connection in this thread is blocking reclamation, we'll never be able to reclaim evicted memory
        return NULL;
    }

    bool given_up;

    // Try to reclaim previously retired memory
    it = force_reclaim_item_alloc(ntotal, id, &given_up);
    if (it != NULL) return it;
    if (given_up) return NULL;

    // Evict all the memory we need
    do_item_evict(ntotal);
    it = force_reclaim_item_alloc(ntotal, id, &given_up);
    if (it != NULL) return it;
    if (given_up) return NULL;

    // Some other thread took the memory we evicted
    for (int i = 0; i < 10; i++) {
        size_t mem_free = get_mem_free();

        while (mem_free >= ntotal) {
            it = do_item_alloc_pull_slabs_alloc();
            if (it != NULL) return it;
            mem_free = get_mem_free();
        }

        size_t mem_needed = ntotal - mem_free;
        do_item_evict(mem_needed);
        it = force_reclaim_item_alloc(ntotal, id, &given_up);
        if (it != NULL) return it;
        if (given_up) return NULL;
    }

    return NULL;
}

/* Chain another chunk onto this chunk. */
/* slab mover: if it finds a chunk without ITEM_CHUNK flag, and no ITEM_LINKED
 * flag, it counts as busy and skips.
 * I think it might still not be safe to do linking outside of the slab lock
 */
item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain) {
    // TODO: Should be a cleaner way of finding real size with slabber calls
    size_t size = bytes_remain + sizeof(item_chunk);
    if (size > settings.slab_chunk_size_max)
        size = settings.slab_chunk_size_max;
    unsigned int id = slabs_clsid(size);
    //struct slab *slab;

    fprintf(stderr, "Chunked items/allocations not yet implemented.\n");
    exit(EXIT_FAILURE);

    item_chunk *nch = (item_chunk *) do_item_alloc_pull(size, id);
    if (nch == NULL) {
        // The final chunk in a large item will attempt to be a more
        // appropriately sized chunk to minimize memory overhead. However, if
        // there's no memory available in the lower slab classes we fail the
        // SET. In these cases as a fallback we ensure we attempt to evict a
        // max-size item and reuse a large chunk.
        if (size == settings.slab_chunk_size_max) {
            return NULL;
        } else {
            size = settings.slab_chunk_size_max;
            id = slabs_clsid(size);
            nch = (item_chunk *) do_item_alloc_pull(size, id);

            if (nch == NULL)
                return NULL;
        }
    }

    //slab = ((item*)nch)->slab;

    // link in.
    // ITEM_CHUNK[ED] bits need to be protected by the slabs lock.
    //!! slabs_mlock();
    nch->head = ch->head;
    ch->next = nch;
    nch->prev = ch;
    nch->next = 0;
    nch->used = 0;
    //nch->slab = slab;
    nch->size = size - sizeof(item_chunk);
    nch->it_flags |= ITEM_CHUNK;
    //!! slabs_munlock();
    return nch;
}

item *do_item_alloc(const char *key, const size_t nkey, const unsigned int flags,
                    const rel_time_t exptime, const int nbytes) {
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    // Avoid potential underflows.
    if (nbytes < 2)
        return 0;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    unsigned int id = slabs_clsid(ntotal);
    unsigned int hdr_id = 0;
#ifdef USE_SLAB_ALLOCATOR
    if (id == 0)
        return 0;
#endif

    /* This is a large item. Allocate a header object now, lazily allocate
     *  chunks while reading the upload.
     */
    if (ntotal > settings.slab_chunk_size_max) {
        fprintf(stderr, "Chunked items/allocations not yet implemented.\n");
        fprintf(stderr, "ntotal: %zu, slab_chunk_size_max: %d\n", ntotal, settings.slab_chunk_size_max);
        exit(EXIT_FAILURE);
        /* We still link this item into the LRU for the larger slab class, but
         * we're pulling a header from an entirely different slab class. The
         * free routines handle large items specifically.
         */
        int htotal = nkey + 1 + nsuffix + sizeof(item) + sizeof(item_chunk);
        if (settings.use_cas) {
            htotal += sizeof(uint64_t);
        }
#ifdef NEED_ALIGN
        // header chunk needs to be padded on some systems
        int remain = htotal % 8;
        if (remain != 0) {
            htotal += 8 - remain;
        }
#endif
        hdr_id = slabs_clsid(htotal);
        it = do_item_alloc_pull(htotal, hdr_id);
        /* setting ITEM_CHUNKED is fine here because we aren't LINKED yet. */
        if (it != NULL)
            it->it_flags = ITEM_CHUNKED;
    } else {
        it = do_item_alloc_pull(ntotal, id);
        if (it != NULL)
            it->it_flags = 0;
    }

    if (it == NULL) {
        return NULL;
    }

    it->it_flags |= settings.use_cas ? ITEM_CAS : 0;
    it->it_flags |= nsuffix != 0 ? ITEM_CFLAGS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;
    memcpy(ITEM_key(it), key, nkey);
    it->exptime = exptime;
    if (nsuffix > 0) {
        memcpy(ITEM_suffix(it), &flags, sizeof(flags));
    }

    /* Initialize internal chunk. */
    if (it->it_flags & ITEM_CHUNKED) {
        item_chunk *chunk = (item_chunk *) ITEM_schunk(it);

        chunk->next = 0;
        chunk->prev = 0;
        chunk->used = 0;
        chunk->size = 0;
        chunk->head = it;
    }

    return it;
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
#ifdef USE_SLAB_ALLOCATOR
    char prefix[40];
    uint8_t nsuffix;
    if (nbytes < 2)
        return false;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
#else
    return true;
#endif
}

item* do_item_link(item *it, const uint64_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);

    //This might conflict if item removed and is being reinserted
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED; 

    //This might conflict as well, but it should be OK to be approximated
    it->time = current_time;
    size_t ntotal = ITEM_ntotal(it);

    item* it_found = assoc_insert(it, hv);
    if (!it_found) {
        t_stats_state.curr_bytes += ntotal;
        t_stats_state.curr_items += 1;
        t_stats.total_items += 1;
    }

    return it_found;
}

void do_item_unlink(item *it, const uint64_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);

    t_stats_state.curr_bytes -= ITEM_ntotal(it);
    t_stats_state.curr_items -= 1;

    assoc_delete(ITEM_key(it), it->nkey, hv);
}

/* Bump the CLOCK value of item's table */
void do_item_update(item *it, const uint64_t hv) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);

    //Update item's last accessed time
    it->time = current_time;
    assoc_bump(it, hv);
}

bool do_item_replace(item *new_it, const uint64_t hv) {
    char* key __attribute__((unused)) = ITEM_key(new_it);
    uint8_t nkey __attribute__((unused)) = new_it->nkey;
    int new_it_nbytes = new_it->nbytes;

    int old_it_nbytes = assoc_replace(new_it, hv);

    MEMCACHED_ITEM_REPLACE(key, nkey, old_it_nbytes, new_it_nbytes);

    if (old_it_nbytes != 0) {
        t_stats_state.curr_bytes += new_it_nbytes - old_it_nbytes;
    }

    return !!old_it_nbytes;
}

bool do_item_set(item *new_it, const uint64_t hv) {
    int new_it_nbytes = new_it->nbytes;

    int old_it_nbytes = assoc_set(new_it, hv);

    t_stats_state.curr_bytes += new_it_nbytes - old_it_nbytes;

    if (old_it_nbytes == 0) {
        t_stats_state.curr_items += 1;
        t_stats.total_items += 1;
    }

    return !!old_it_nbytes;
}

/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint64_t hv, LIBEVENT_THREAD *t, const bool do_update) {
    item *it = assoc_find(key, nkey, hv);

    int was_found = 0;

    if (it != NULL) {
        was_found = 1;
        if (item_is_flushed(it)) {
            //Cache was flushed, items present before the flush should be removed
            do_item_unlink(it, hv);

            it = NULL;

            t->stats.get_flushed++;

            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by flush");
            }
            was_found = 2;

        } else if (it->exptime != 0 && it->exptime <= current_time) {
            //Item's ttl has expired, remove it
            do_item_unlink(it, hv);
            it = NULL;
            t->stats.get_expired++;
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by expire");
            }
            was_found = 3;

        } else {
            //Item is present and shall remain so, bump it in the LRU
            if (do_update) {
                do_item_bump(t, it, hv);
            }
        }
    }

    if (settings.verbose > 2) {
        int ii;
        if (!was_found) {
            fprintf(stderr, "> NOT FOUND ");
        } else {
            fprintf(stderr, "> FOUND KEY ");
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
        fprintf(stderr, "\n");
    }

    /* For now this is in addition to the above verbose logging. */
    LOGGER_LOG(t->l, LOG_FETCHERS, LOGGER_ITEM_GET, NULL, was_found, key,
               nkey, (it) ? it->nbytes : 0, /*(it) ? ITEM_clsid(it) : 0*/ 0, t->cur_sfd);

    return it;
}

// Requires lock held for item.
// Split out of do_item_get() to allow mget functions to look through header
// data before losing state modified via the bump function.
void do_item_bump(LIBEVENT_THREAD *t, item *it, const uint64_t hv) {
    it->it_flags |= ITEM_FETCHED;
    do_item_update(it, hv);
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint64_t hv, LIBEVENT_THREAD *t) {
    item *it = do_item_get(key, nkey, hv, t, DO_UPDATE);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}
