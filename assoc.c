#ifndef USE_ASSOC_NBLIST

/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "assoc_bucket.h"


extern __thread int gt_tid;


/* Main hash table. This is where we look except during expansion. */
static bucket_t** hashtable = 0;
static bucket_t** new_hashtable = 0; /* hash table that is being expanded into */

/* how many powers of 2's worth of buckets we use */
/*atomic*/ unsigned int hashpower = HASHPOWER_DEFAULT;
static /*atomic*/ unsigned int new_hashpower;


/* Clock related */
static __thread size_t t_hand;

#ifdef ASSOC_CLOCK_NO_PADDING
typedef uint64_t clock_assoc_t;
#define GET_CLOCK_VALUE(arr, idx) ((arr)[idx])
#else
typedef struct {
    uint64_t value;
    char padding[L1_DCACHE_LINE_SIZE_DEFAULT - sizeof(uint64_t)];
} __attribute__((aligned(L1_DCACHE_LINE_SIZE_DEFAULT))) clock_assoc_t;

_Static_assert(sizeof(clock_assoc_t) == L1_DCACHE_LINE_SIZE_DEFAULT, "clock_assoc_t size must be equal to L1_DCACHE_LINE_SIZE_DEFAULT");

#define GET_CLOCK_VALUE(arr, idx) ((arr)[idx].value)
#endif

static clock_assoc_t* clock_arr = NULL;
static clock_assoc_t* new_clock_arr = NULL;

size_t assoc_clock_frequency = 1 << CLOCK_FREQUENCY_POWER_DEFAULT;
static __thread size_t t_clock_cycle = 0;

_Static_assert(sizeof(curr_items_t) == L1_DCACHE_LINE_SIZE_DEFAULT, "curr_items_t size must be equal to L1_DCACHE_LINE_SIZE_DEFAULT");

/* atomic */ curr_items_t* curr_items = NULL; 
#define t_curr_items (curr_items[gt_tid].value)

/* Maintenence thread  / expansion */
static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static /*atomic*/ bool expanding = false;

/* CLOCK ageing thread */
static pthread_cond_t clock_ageing_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t clock_ageing_lock = PTHREAD_MUTEX_INITIALIZER;
// value to age CLOCK by
static uint64_t g_clock_age_decrement;

void notify_clock_ageing_thread(uint64_t decrement);


void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;
    }

    size_t hash_size = hashsize(hashpower);

    //Allocate space for hashsize lists
    hashtable = malloc(hash_size * sizeof(bucket_t*));
    if (!hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }

    size_t arena_size = 1 << settings.assoc_arena_size_power;
    assoc_bucket_init(arena_size);

    //Initialize a list for each hashtable collision list
    for (int i = 0; i < hash_size; ++i) {
        hashtable[i] = new_bucket();
    }

    clock_arr = l1_dcache_line_aligned_calloc(hash_size, sizeof(clock_assoc_t));
    if (!clock_arr) {
        fprintf(stderr, "Failed to init CLOCK values.\n");
        exit(EXIT_FAILURE);
    } else if ((uintptr_t)clock_arr % L1_DCACHE_LINE_SIZE_DEFAULT) {
        fprintf(stderr, "CLOCK array not properly aligned to cache line.\n");
        exit(EXIT_FAILURE);
    }

    assoc_clock_frequency = 1 << settings.clock_frequency_power;

    //Allocate array that keeps track of total number of items
    curr_items = l1_dcache_line_aligned_calloc(settings.num_threads + 1, sizeof(curr_items_t));
    if (!curr_items) {
        fprintf(stderr, "Failed to init curr_items array.\n");
        exit(EXIT_FAILURE);
    } else if ((uintptr_t)curr_items % L1_DCACHE_LINE_SIZE_DEFAULT) {
        fprintf(stderr, "curr_items array not properly aligned to cache line.\n");
        exit(EXIT_FAILURE);
    }

    stats_state.assoc.hash_power_level = hashpower;
    stats_state.assoc.hash_bytes = hashsize(hashpower) * (sizeof(void *) + sizeof(clock_assoc_t));
}

static inline void inc_clock(clock_assoc_t* l_clock_arr, uint64_t bucket) {
    if (++t_clock_cycle != assoc_clock_frequency) {
        return;
    }

    t_clock_cycle = 0;
#ifdef ASSOC_CLOCK_UNSYNCHRONIZED
    GET_CLOCK_VALUE(l_clock_arr, bucket)++;
#else
    ATOMIC_INC(GET_CLOCK_VALUE(l_clock_arr, bucket));
#endif
}

item *assoc_find(const char *key, const size_t nkey, const uint64_t hv) {
    bucket_t** l_hashtable;
    item *it;
    uint64_t hmask;
    bucket_t* bucket;
    clock_assoc_t* l_clock_arr;
    uint16_t hv_msb = HV_16MSB(hv);
    // because we aren't using else, expanding may change between the two branches, causing none to be accessed
    bool l_expanding = AALOAD(expanding);

    if (l_expanding) {
        hmask = hv & hashmask(ALOAD(new_hashpower));
        l_hashtable = recl_protect_push_load_mem(new_hashtable, __ATOMIC_RELAXED);
        bucket = l_hashtable[hmask];
        //Dont change CLOCK while expanding
        recl_unprotect_all_mem();
        it = get(bucket, key, nkey, hv_msb);
    }

    if (!l_expanding || !it) {
        hmask = hv & hashmask(ALOAD(hashpower));
        recl_protect_push_load_mem2(l_hashtable, hashtable, l_clock_arr, clock_arr, __ATOMIC_RELAXED);
        bucket = l_hashtable[hmask];
        inc_clock(l_clock_arr, hmask);
        recl_unprotect_all_mem();
        it = get(bucket, key, nkey, hv_msb);
    }

    MEMCACHED_ASSOC_FIND(key, nkey);
    return it;
}

item* assoc_insert(item *it, const uint64_t hv) {
    bucket_t** l_hashtable;
    clock_assoc_t* l_clock_arr;
    bucket_t* bucket;
    uint64_t hmask;

    if(AALOAD(expanding)) {
        l_hashtable = recl_protect_push_load_mem(new_hashtable, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(new_hashpower));
        bucket = l_hashtable[hmask];
        //Dont change CLOCK while expanding
    } else {
        hmask = hv & hashmask(ALOAD(hashpower));
        recl_protect_push_load_mem2(l_hashtable, hashtable, l_clock_arr, clock_arr, __ATOMIC_RELAXED);
        bucket = l_hashtable[hmask];
        inc_clock(l_clock_arr, hmask);
    }

    recl_unprotect_all_mem();

    item* it_found = insert(bucket, it, HV_16MSB(hv));
    if(!it_found) {
        ASTORE(t_curr_items, t_curr_items + 1);
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey);
    return it_found;
}

int assoc_delete(const char *key, const size_t nkey, const uint64_t hv) {
    bucket_t* bucket;
    bucket_t** l_hashtable;
    uint64_t hmask;
    uint16_t hv_msb = HV_16MSB(hv);
    bool found;
    bool l_expanding = AALOAD(expanding);

    l_hashtable = recl_protect_push_load_mem(hashtable, __ATOMIC_RELAXED);
    hmask = hv & hashmask(ALOAD(hashpower));
    bucket = l_hashtable[hmask];
    recl_unprotect_all_mem();
    found = del(bucket, key, nkey, hv_msb, true);

    if (l_expanding) {
        // race condition: may have no effect and return a false negative if performed between expansion's delete and insert
        l_hashtable = recl_protect_push_load_mem(new_hashtable, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(new_hashpower));
        bucket = l_hashtable[hmask];
        recl_unprotect_all_mem();
        found = del(bucket, key, nkey, hv_msb, true) || found;
    }

    if(found) {
        ASTORE(t_curr_items, t_curr_items - 1);

#ifdef RECL_EBR // too expensive to do this micro-optimization outside of EBR
        if (!l_expanding && is_empty(bucket)) {
            ASTORE(GET_CLOCK_VALUE(clock_arr, hmask), 0);
        }
#endif
    }

    return !found;
}

int assoc_replace(item *new_it, const uint64_t hv) {
    bucket_t** l_hashtable;
    clock_assoc_t* l_clock_arr;
    bucket_t* bucket;
    uint64_t hmask;

    if(AALOAD(expanding)) {
        l_hashtable = recl_protect_push_load_mem(new_hashtable, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(new_hashpower));
        bucket = l_hashtable[hmask];
        //Dont change CLOCK while expanding
    } else {
        recl_protect_push_load_mem2(l_hashtable, hashtable, l_clock_arr, clock_arr, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(hashpower));
        bucket = l_hashtable[hmask];
        inc_clock(l_clock_arr, hmask);
    }

    recl_unprotect_all_mem();

    // if we're replacing, new_it's key, nkey, and hv should be the same as old_it's
    int ret = replace(bucket, ITEM_key(new_it), new_it->nkey, HV_16MSB(hv), new_it);

    return ret;
}

int assoc_set(item *new_it, const uint64_t hv) {
    bucket_t** l_hashtable;
    clock_assoc_t* l_clock_arr;
    bucket_t* bucket;
    uint64_t hmask;

    if(AALOAD(expanding)) {
        l_hashtable = recl_protect_push_load_mem(new_hashtable, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(new_hashpower));
        bucket = l_hashtable[hmask];
        //Dont change CLOCK while expanding
    } else {
        recl_protect_push_load_mem2(l_hashtable, hashtable, l_clock_arr, clock_arr, __ATOMIC_RELAXED);
        hmask = hv & hashmask(ALOAD(hashpower));
        bucket = l_hashtable[hmask];
        inc_clock(l_clock_arr, hmask);
    }

    recl_unprotect_all_mem();

    // if we're setting, new_it's key, nkey, and hv should be the same as old_it's
    int ret = set(bucket, ITEM_key(new_it), new_it->nkey, HV_16MSB(hv), new_it);
    if (ret == 0) {
        ASTORE(t_curr_items, t_curr_items + 1);
    }

    return ret;
}


void assoc_bump(item *it, const uint64_t hv) {
    //Dont change CLOCK while expanding
    if (!AALOAD(expanding)) {
        uint64_t hmask = hv & hashmask(ALOAD(hashpower));
        clock_assoc_t* l_clock_arr = recl_protect_push_load_mem(clock_arr, __ATOMIC_RELAXED);
        inc_clock(l_clock_arr, hmask);
        recl_unprotect_all_mem();
    }
}

#define CLOCK_VAL_MIN_ARR_SIZE 4

/* Returns number of items removed. */
int try_evict(const uint64_t total_bytes, size_t *mem_freed) {
    bucket_t** l_hashtable;
    clock_assoc_t* l_clock_arr;
    int removed_total = 0;
    size_t freed_bytes_total = 0;
    size_t num_buckets = hashsize(ALOAD(hashpower));
    uint64_t clock_age_decrement = 0;

    uint64_t clock_val_mins[CLOCK_VAL_MIN_ARR_SIZE];
    size_t bucket_ids_to_evict[CLOCK_VAL_MIN_ARR_SIZE];
    for (int i = 0; i < CLOCK_VAL_MIN_ARR_SIZE; i++) {
        clock_val_mins[i] = UINT64_MAX;
        bucket_ids_to_evict[i] = SIZE_MAX;
    }

    // Find non-empty buckets with the lowest CLOCK value
    size_t hand_start = t_hand;
    recl_protect_push_load_mem2(l_hashtable, hashtable, l_clock_arr, clock_arr, __ATOMIC_RELAXED);
    do {
        uint64_t clock_val = ALOAD(GET_CLOCK_VALUE(l_clock_arr, t_hand));
        if (is_empty(l_hashtable[t_hand]) || clock_val >= clock_val_mins[CLOCK_VAL_MIN_ARR_SIZE - 1]) {
            goto loop_continue;
        }

        // Immediately evict any buckets with CLOCK value 1 or less
        if (clock_val <= 1) {
            size_t freed_bytes;
            int items_cleared = clear(l_hashtable[t_hand], &freed_bytes);
            removed_total += items_cleared;
            ASTORE(t_curr_items, t_curr_items - items_cleared);
            freed_bytes_total += freed_bytes;
            ASTORE(GET_CLOCK_VALUE(l_clock_arr, t_hand), 0);
            if (freed_bytes_total >= total_bytes) {
                goto done;
            } else {
                goto loop_continue;
            }
        }

        int i;
        for (i = CLOCK_VAL_MIN_ARR_SIZE - 2; i >= 0 && clock_val < clock_val_mins[i]; i--) {
            clock_val_mins[i + 1] = clock_val_mins[i];
            bucket_ids_to_evict[i + 1] = bucket_ids_to_evict[i];
        }

        clock_val_mins[i + 1] = clock_val;
        bucket_ids_to_evict[i + 1] = t_hand;

loop_continue:
        t_hand++;
        if (__glibc_unlikely(t_hand >= num_buckets)) {
            t_hand = 0;
        }
    } while (t_hand != hand_start);

    for (int i = 0; i < CLOCK_VAL_MIN_ARR_SIZE; i++) {
        if (bucket_ids_to_evict[i] == SIZE_MAX) break;
        if (is_empty(l_hashtable[bucket_ids_to_evict[i]])) continue;

        size_t freed_bytes;
        int items_cleared = clear(l_hashtable[bucket_ids_to_evict[i]], &freed_bytes);
        removed_total += items_cleared;
        ASTORE(t_curr_items, t_curr_items - items_cleared);
        freed_bytes_total += freed_bytes;
        ASTORE(GET_CLOCK_VALUE(l_clock_arr, bucket_ids_to_evict[i]), 0);
        clock_age_decrement = clock_val_mins[i];

        if (freed_bytes_total >= total_bytes) {
            goto done;
        }
    }

done:
    recl_unprotect_all_mem();
    notify_clock_ageing_thread(clock_age_decrement);
    *mem_freed = freed_bytes_total;
    return removed_total;
}

uint64_t get_curr_items() {
    int64_t res = 0;
    for(int i = 0; i < settings.num_threads + 1; i++)
        res += ALOAD(curr_items[i].value);
    return (uint64_t) res;
}

int start_assoc_maintenance_thread(void) {
    int ret;
    pthread_t thread;

    if((ret = pthread_create(&thread, NULL, assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Failed to start maintenance thread: %s\n", strerror(ret));
        return -1;
    }

    return 0;
}

/* Check if we should expand hash table */
void assoc_check_expand() {
    if (pthread_mutex_trylock(&maintenance_lock) == 0) {
        uint64_t curr = get_curr_items();

        /* If there are 1.5 more items than there are buckets, expand */
        if (curr > (hashsize(hashpower) * 3) / 2 && hashpower < HASHPOWER_MAX) {
            pthread_cond_signal(&maintenance_cond);
        }
        pthread_mutex_unlock(&maintenance_lock);
    }
}

void start_expansion() {
    new_hashpower = hashpower + 1;

    new_clock_arr = calloc(hashsize(new_hashpower), sizeof(clock_assoc_t));
    if (!new_clock_arr) {
        return;
    }

    //Allocate space for hashsize lists
    new_hashtable = calloc(hashsize(new_hashpower), sizeof(bucket_t*));
    if (new_hashtable) {

        //Transfer existing buckets to new hashtable
        for (unsigned int i = 0; i < hashsize(hashpower); ++i)
            new_hashtable[i] = hashtable[i];

        //Create new buckets
        for (unsigned int i = hashsize(hashpower); i < hashsize(new_hashpower); ++i)
            new_hashtable[i] = new_bucket();

        //Threads can now insert into new hash table
        ARSTORE(expanding, true);

        //TODO: lookups and deletes must be done in both "hashpowers" during expansion

        if(settings.verbose > 0)
            fprintf(stderr, "Starting expansion from %d to %d\n", hashpower, new_hashpower);
    } else {
        free(new_clock_arr);
    }

    stats_state.assoc.hash_power_level = new_hashpower;
    stats_state.assoc.hash_bytes = hashsize(new_hashpower) * (sizeof(void *) + sizeof(clock_assoc_t));
    stats_state.assoc.hash_is_expanding = true;
}


#define ASSOC_MAINTENENCE_THREAD_SLEEP 10000

void *assoc_maintenance_thread(void *arg) {
    gt_tid = settings.num_threads + 1;
    recl_thread_init(gt_tid, 4);
#ifdef RECL_HP
    // This is a weird case because in this HP implementation, connections protect items, not threads.
    // Since this isn't a worker thread, it won't have any connections, but it may need to protect items during migration.
    conn fake_conn = {0};
    recl_setup_connection_item_announcements(&fake_conn);
    recl_set_curr_item_announcements_connection(&fake_conn);
#endif

    //Required for cond signal to work (and unlock this)
    mutex_lock(&maintenance_lock);
    bool expanded_last_iter = false;

    while(true) {

        //Do not expand twice in a row (without waiting for cond)
        if(expanding && !expanded_last_iter) {
            expanded_last_iter = true;

            //Wait for two epochs, so that no thread thinks the
            //  old hashtable is the current hashtable and inserts
            //  new items there
#ifdef RECL_EBR
            size_t bags_freed = 0;
            while (bags_freed < NUM_LIMBO_BAGS) {
                bags_freed += ebr_force_announce_epoch();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
            ebr_leave_quiescent();
#elif defined(RECL_HP)
// fazer isto para hp, mas e dificil porque nao se sabe quando e que se deixa de aceder
// ao contrario do que e feito la em baixo, nao se tem a hash table reclaimed para saber quando e que as threads ja nao estao a aceder
            sleep(1); // placeholder, deve funcionar por agora
#elif defined(RECL_QSENSE)
            qsense_mode_e mode = qsense_get_protection_mode();
            if (mode == QSENSE_EBR) {
                size_t bags_freed = 0;
                while (bags_freed < NUM_LIMBO_BAGS) {
                    bags_freed += ebr_force_announce_epoch();
                    usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
                }
                ebr_leave_quiescent();
            } else if (mode == QSENSE_HP) {
                // Unlike when operating with pure HPs, we can assert that no thread thinks
                // the old hashtable is current hash table by waiting for system quiescence
                qsense_wait_for_system_quiescence();
            } else {
                // should never happen
                fprintf(stderr, "Unknown QSense mode in assoc_maintenance_thread: %d\n", mode);
                exit(EXIT_FAILURE);
            }
#else
#error "Must define either RECL_EBR or RECL_HP for memory reclamation"
#endif

            // no need to protect hashtable, only we can free it
            migrate_expanded_items(hashtable, new_hashtable, hashpower, new_hashpower);

            // Can be concurrently modified by clock ageing thread
            clock_assoc_t* l_clock_arr = clock_arr;
            while (!__atomic_compare_exchange_n(&clock_arr, &l_clock_arr, new_clock_arr,
                        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}

            recl_retire_mem(hashtable);
            recl_retire_mem(l_clock_arr);

            ASTORE(hashpower, new_hashpower);
            ASTORE(hashtable, new_hashtable);

            //Finish expanding
            recl_enter_quiescent();
            
            stats_state.assoc.hash_bytes -= hashsize(hashpower - 1) * (sizeof(void *) + sizeof(clock_assoc_t));
            stats_state.assoc.hash_is_expanding = false;
            
            ARSTORE(expanding, false);

            if(settings.verbose > 0) {
                fprintf(stderr, "Expansion ended\n");
            }

            //Try and advance 2 epochs again, so that 
			//	we reclaim the hash table and the clock array
			//	during the hash table process
#ifdef RECL_EBR
			bags_freed = 0;
            while(bags_freed < NUM_LIMBO_BAGS) {
                bags_freed += ebr_force_announce_epoch();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
#elif defined(RECL_HP)
            // We don't want to hold item memory forever, so release it ASAP
            while (t_recl_hp->retired_items.size > 0) {
                hp_reclaim_items();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }

            while (t_recl_hp->retired_mem.size > 0) {
                hp_reclaim_mem();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
#elif defined(RECL_QSENSE)
            mode = qsense_get_protection_mode();
            if (mode == QSENSE_EBR) {
                size_t bags_freed = 0;
                while (bags_freed < NUM_LIMBO_BAGS) {
                    bags_freed += ebr_force_announce_epoch();
                    usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
                }
                ebr_leave_quiescent();
            } else if (mode == QSENSE_HP) {
                while (t_recl_hp->retired_items.size > 0) {
                    hp_reclaim_items();
                    usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
                }

                while (t_recl_hp->retired_mem.size > 0) {
                    hp_reclaim_mem();
                    usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
                }
            } else {
                // should never happen
                fprintf(stderr, "Unknown QSense mode in assoc_maintenance_thread: %d\n", mode);
                exit(EXIT_FAILURE);
            }
#else
#error "Must define either RECL_EBR or RECL_HP for memory reclamation"
#endif
        } else {
            expanded_last_iter = false;
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);
            start_expansion();
        }
    }

    mutex_unlock(&maintenance_lock);
    return NULL;
}

#define CLOCK_AGEING_THREAD_SLEEP_INTERVAL 10

static void *clock_ageing_thread(void *arg) {
    gt_tid = settings.num_threads;
    recl_thread_init(gt_tid, 1);

    while(true) {
        mutex_lock(&clock_ageing_lock);
        pthread_cond_wait(&clock_ageing_cond, &clock_ageing_lock);

        if (settings.verbose > 0) {
            fprintf(stderr, "Starting CLOCK ageing by %llu\n", (unsigned long long) g_clock_age_decrement);
        }

        recl_leave_quiescent();

        unsigned int l_hashpower = ALOAD(hashpower);
        clock_assoc_t* l_clock_arr = recl_protect_push_load_mem(clock_arr, __ATOMIC_RELAXED);

        // No point in ageing CLOCK until expansion is done
        // if (l_hashpower != hashpower) it means there was an expansion and l_hashpower may not correspond to l_clock_arr's size
        if (AALOAD(expanding) || l_hashpower != ALOAD(hashpower)) {
            recl_enter_quiescent();
            mutex_unlock(&clock_ageing_lock);
            continue;
        }

        size_t clock_len = hashsize(l_hashpower);
        uint64_t clock_age_decrement = g_clock_age_decrement;
        clock_assoc_t* aged_clock_arr = l1_dcache_line_aligned_alloc(clock_len * sizeof(clock_assoc_t));
        if (!aged_clock_arr) {
            recl_enter_quiescent();
            mutex_unlock(&clock_ageing_lock);
            continue;
        }

        memcpy(aged_clock_arr, l_clock_arr, clock_len * sizeof(clock_assoc_t));

        // Age all CLOCK values
        for (size_t i = 0; i < clock_len; i++) {
            uint64_t old_clock_val = GET_CLOCK_VALUE(aged_clock_arr, i);
            uint64_t new_clock_val = (old_clock_val > clock_age_decrement) ? (old_clock_val - clock_age_decrement) : 0;
            GET_CLOCK_VALUE(aged_clock_arr, i) = new_clock_val;
        }

        recl_unprotect_all_mem();

        if (__atomic_compare_exchange_n(&clock_arr, &l_clock_arr, aged_clock_arr, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            recl_retire_mem(l_clock_arr);
        } else {
            // Expansion occurred and already retired l_clock_arr
            free(aged_clock_arr);
        }

        recl_enter_quiescent();

        // Wait for two epochs, so that no thread thinks the
        // old CLOCK array is the current one and orders ageing
        // based on it with a bad clock_age_decrement
        // Also assures that the retired CLOCK array is reclaimed
#ifdef RECL_EBR
        size_t bags_freed = 0;
        while (bags_freed < NUM_LIMBO_BAGS) {
            bags_freed += ebr_force_announce_epoch();
            usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
        }
#elif defined(RECL_HP)
        while (hp_reclaim_mem() == 0) {
            usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
        }
#elif defined(RECL_QSENSE)
        qsense_mode_e mode = qsense_get_protection_mode();
        if (mode == QSENSE_EBR) {
            size_t bags_freed = 0;
            while (bags_freed < NUM_LIMBO_BAGS) {
                bags_freed += ebr_force_announce_epoch();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
            ebr_leave_quiescent();
        } else if (mode == QSENSE_HP) {
            while (hp_reclaim_mem() == 0) {
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
        } else {
            // should never happen
            fprintf(stderr, "Unknown QSense mode in clock_ageing_thread: %d\n", mode);
            exit(EXIT_FAILURE);
        }
#else
#error "Must define either RECL_EBR or RECL_HP for memory reclamation"
#endif

        // Prevent too frequent ageing
        sleep(CLOCK_AGEING_THREAD_SLEEP_INTERVAL);

        if (settings.verbose > 0) {
            fprintf(stderr, "CLOCK ageing ended\n");
        }

        mutex_unlock(&clock_ageing_lock);
    }

    return NULL;
}

int start_clock_ageing_thread(void) {
    int ret;
    pthread_t thread;

    if((ret = pthread_create(&thread, NULL, clock_ageing_thread, NULL)) != 0) {
        fprintf(stderr, "Failed to start CLOCK ageing thread: %s\n", strerror(ret));
        return -1;
    }

    return 0;
}

void notify_clock_ageing_thread(uint64_t decrement) {
    // myTODO: think of a better threshold
    if (decrement <= 1) {
        return;
    }

    if (pthread_mutex_trylock(&clock_ageing_lock) == 0) {
        g_clock_age_decrement = decrement;
        pthread_cond_signal(&clock_ageing_cond);
        pthread_mutex_unlock(&clock_ageing_lock);
    }
}

#else

__attribute__((unused)) static const char assoc_c_empty_file_warning;

#endif