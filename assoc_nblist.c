#ifdef USE_ASSOC_NBLIST

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
#include "nblist.h"


extern __thread int gt_tid;

//Amount of collision lists
#define hashsize(n) ((uint64_t)1<<(n))

//Masks to know to what collision list an item goes to
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
static List** hashtable = 0;
static List** new_hashtable = 0; /* hash table that is being expanded into */

/* how many powers of 2's worth of buckets we use */
/*atomic*/ unsigned int hashpower = HASHPOWER_DEFAULT;
static /*atomic*/ unsigned int new_hashpower;


/* Clock related */
static __thread size_t t_hand;

#ifdef ASSOC_CLOCK_PADDED
typedef struct {
    uint64_t value;
    char padding[L1_DCACHE_LINE_SIZE_DEFAULT - sizeof(uint64_t)];
} __attribute__((aligned(L1_DCACHE_LINE_SIZE_DEFAULT))) clock_assoc_t;

_Static_assert(sizeof(clock_assoc_t) == L1_DCACHE_LINE_SIZE_DEFAULT, "clock_assoc_t size must be equal to L1_DCACHE_LINE_SIZE_DEFAULT");

#define GET_CLOCK_VALUE(arr, idx) ((arr)[idx].value)
#else
typedef uint64_t clock_assoc_t;
#define GET_CLOCK_VALUE(arr, idx) ((arr)[idx])
#endif

static clock_assoc_t* clock_arr = NULL;
static clock_assoc_t* new_clock_arr = NULL;

size_t assoc_clock_frequency = 1 << CLOCK_FREQUENCY_POWER_DEFAULT;
static __thread size_t t_clock_cycle = 0;

typedef struct {
    int64_t value;
    char padding[L1_DCACHE_LINE_SIZE_DEFAULT - sizeof(int64_t)];
} __attribute__((aligned(L1_DCACHE_LINE_SIZE_DEFAULT))) curr_items_t;

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

item* dummy;

void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;
    }

    dummy = calloc(1, sizeof(item) + 8);
    dummy->nkey = 1;
    dummy->nbytes = 1;
    dummy->data[0].cas = 0x6100610d0a000000;

    size_t hash_size = hashsize(hashpower);

    //Allocate space for hashsize lists
    hashtable = malloc(hash_size * sizeof(List*));
    if (!hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }

    //Initialize a list for each hashtable collision list
    for (int i = 0; i < hash_size; ++i) {
        hashtable[i] = new_nblist();
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

static inline void inc_clock(uint64_t bucket) {
    if (++t_clock_cycle != assoc_clock_frequency) {
        return;
    }

    t_clock_cycle = 0;
#ifdef ASSOC_CLOCK_UNSYNCHRONIZED
    GET_CLOCK_VALUE(clock_arr, bucket)++;
#else
    ATOMIC_INC(GET_CLOCK_VALUE(clock_arr, bucket));
#endif
}

item *assoc_find(const char *key, const size_t nkey, const uint64_t hv) {
    item *it;
    uint64_t hmask;
    List* list;
    // because we aren't using else, expanding may change between the two branches, causing none to be accessed
    bool l_expanding = AALOAD(expanding);

    if (l_expanding) {
        hmask = hv & hashmask(ALOAD(new_hashpower));
        list = new_hashtable[hmask];
        //Dont change CLOCK while expanding
        it = get(list, key, nkey);
    }

    if (!l_expanding || !it) {
        hmask = hv & hashmask(ALOAD(hashpower));
        list = hashtable[hmask];
        inc_clock(hmask);
        it = get(list, key, nkey);
    }

    MEMCACHED_ASSOC_FIND(key, nkey);
    return it;
}

item* assoc_insert(item *it, const uint64_t hv) {
    List* list;
    uint64_t hmask;

    if(AALOAD(expanding)) {
        hmask = hv & hashmask(ALOAD(new_hashpower));
        list = new_hashtable[hmask];
        //Dont change CLOCK while expanding
    } else {
        hmask = hv & hashmask(ALOAD(hashpower));
        list = hashtable[hmask];
        inc_clock(hmask);
    }


    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey);

    bool inserted = insert(list, it);
    if(inserted) {
        ASTORE(t_curr_items, t_curr_items + 1);
    }

    return inserted ? NULL : dummy;
}

int assoc_delete(const char *key, const size_t nkey, const uint64_t hv) {
    List* list;
    uint64_t hmask;
    bool found;
    bool l_expanding = AALOAD(expanding);

    hmask = hv & hashmask(ALOAD(hashpower));
    list = hashtable[hmask];
    del(list, key, nkey, true, &found);

    if (l_expanding) {
        // race condition: may have no effect and return a false negative if performed between expansion's delete and insert
        hmask = hv & hashmask(ALOAD(new_hashpower));
        list = new_hashtable[hmask];
        bool found2;
        del(list, key, nkey, true, &found2);
        found = found || found2;
    }

    if(found) {
        ASTORE(t_curr_items, t_curr_items - 1);

        if (!l_expanding && is_empty(list)) {
            ASTORE(GET_CLOCK_VALUE(clock_arr, hmask), 0);
        }
    }

    return !found;
}

int assoc_replace(item *new_it, const uint64_t hv) {
    List* list;
    uint64_t hmask;

    if(AALOAD(expanding)) {
        hmask = hv & hashmask(ALOAD(new_hashpower));
        list = new_hashtable[hmask];
        //Dont change CLOCK while expanding
    } else {
        hmask = hv & hashmask(ALOAD(hashpower));
        list = hashtable[hmask];
        inc_clock(hmask);
    }

    // if we're replacing, new_it's key, nkey, and hv should be the same as old_it's
    bool inserted;
    item* old_it = replace(list, ITEM_key(new_it), new_it->nkey, new_it, true, &inserted);
    ASTORE(t_curr_items, t_curr_items + 1);
    return old_it ? old_it->nbytes : 0;
}

int assoc_set(item *new_it, const uint64_t hv) {
    return assoc_replace(new_it, hv);
}


void assoc_bump(item *it, const uint64_t hv) {
    //Dont change CLOCK while expanding
    if (!AALOAD(expanding)) {
        uint64_t hmask = hv & hashmask(ALOAD(hashpower));
        inc_clock(hmask);
    }
}

#define CLOCK_VAL_MIN_ARR_SIZE 4

/* Returns number of items removed. */
int try_evict(const uint64_t total_bytes, size_t *mem_freed) {
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
    clock_assoc_t* l_clock_arr = clock_arr;
    List** l_hashtable = hashtable;
    do {
        uint64_t clock_val = ALOAD(GET_CLOCK_VALUE(l_clock_arr, t_hand));
        if (is_empty(l_hashtable[t_hand]) || clock_val >= clock_val_mins[CLOCK_VAL_MIN_ARR_SIZE - 1]) {
            goto loop_continue;
        }

        // Immediately evict any buckets with CLOCK value 1 or less
        if (clock_val <= 1) {
            size_t freed_bytes = 0;
            int items_cleared = cleanup(l_hashtable[t_hand], &freed_bytes);
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

        size_t freed_bytes = 0;
        int items_cleared = cleanup(l_hashtable[bucket_ids_to_evict[i]], &freed_bytes);
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
    new_hashtable = calloc(hashsize(new_hashpower), sizeof(List*));
    if (new_hashtable) {

        //Transfer existing buckets to new hashtable
        for (unsigned int i = 0; i < hashsize(hashpower); ++i)
            new_hashtable[i] = hashtable[i];

        //Create new buckets
        for (unsigned int i = hashsize(hashpower); i < hashsize(new_hashpower); ++i)
            new_hashtable[i] = new_nblist();

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
    gt_tid = settings.num_threads;
    recl_thread_init(gt_tid, 4);

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
            size_t bags_freed = 0;
            while (bags_freed < NUM_LIMBO_BAGS) {
                bags_freed += recl_force_announce_epoch();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }

            recl_leave_quiescent();

            for(uint32_t i = 0; i < hashsize(hashpower); ++i) {
                item *head, *tail, *it, *next;

                List *l = hashtable[i]; //old hash table
                head = l->head;
                tail = l->tail;

                //Traverse items in bucket
                for(it = head->next; it != tail; it = next) {
                    char *key = ITEM_key(it);
                    uint8_t size = it->nkey;
                    uint32_t item_hash = hash(key, size);
                    uint32_t new_bucket = item_hash & hashmask(new_hashpower);

                    //Read next now because if we reinsert it will change
                    next = (item*) get_unmarked_reference(it->next);

                    if(i != new_bucket) {
                        //hash mask's left most bit is not 0, change item's bucket

                        List *new_list = new_hashtable[new_bucket];

                        //During this time, the item is not visible
                        //There is also the chance that an item is marked and deleted
                        //  by anoter thread, deleting it permanently
                        //TODO: maybe mark the 2nd least significant bit as well
                        //  to prevent this from happening?

                        bool unused, ret;
                        if(del(l, key, size, false, &unused)) { //TODO: This can be optimized, we already searched
                            ret = insert(new_list, it);

							if(!ret) {
								//Item not inserted for whatever reason,
								//	should be safe to reclaim as it is 
								//	not accessible through the data structure.

								//TODO: test this (?)
       							recl_retire_item(it);

                            	if(settings.verbose > 0) {
                            	    printf("item %.*s, was already in %d\n",
                            	        it->nkey, ITEM_key(it), new_bucket);
                            	}


							} else if(settings.verbose > 1) {
                                printf("Replaced item %.*s, from bucket %d to %d\n",
                                    it->nkey, ITEM_key(it), i, new_bucket);
                            }
                        }
                    }
                }

            }

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
			bags_freed = 0;
            while(bags_freed < NUM_LIMBO_BAGS) {
                bags_freed += recl_force_announce_epoch();
                usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
            }
        } else {
            expanded_last_iter = false;
            pthread_cond_wait(&maintenance_cond, &maintenance_lock);
            start_expansion();
        }
    }

    mutex_unlock(&maintenance_lock);
    return NULL;
}

#define CLOCK_AGEING_THREAD_SLEEP_INTERVAL 30

static void *clock_ageing_thread(void *arg) {
    gt_tid = settings.num_threads + 1;
    recl_thread_init(gt_tid, 1);

    while(true) {
        mutex_lock(&clock_ageing_lock);
        pthread_cond_wait(&clock_ageing_cond, &clock_ageing_lock);

        recl_leave_quiescent();

        unsigned int l_hashpower = ALOAD(hashpower);
        clock_assoc_t* l_clock_arr = ALOAD(clock_arr);

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
        size_t bags_freed = 0;
        while (bags_freed < NUM_LIMBO_BAGS) {
            bags_freed += recl_force_announce_epoch();
            usleep(ASSOC_MAINTENENCE_THREAD_SLEEP);
        }

        // Prevent too frequent ageing
        sleep(CLOCK_AGEING_THREAD_SLEEP_INTERVAL);

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

__attribute__((unused)) static const char assoc_nblist_empty_file_warning;

#endif