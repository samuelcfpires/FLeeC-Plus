#include "recl.h"

#if defined(RECL_HP) || defined(RECL_QSENSE)

#include "bag.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifdef RECL_QSENSE
// Only allow reclamation if currently in HP mode (not switching), or during a switch to EBR mode 
#define can_reclaim() __extension__ ({ \
    qsense_state_t curr_state = get_qsense_state(); \
    ((!curr_state.switching && curr_state.mode == QSENSE_HP) || (curr_state.switching && curr_state.mode == QSENSE_EBR)); \
})
#else
#define can_reclaim() (true)
#endif

#define MAX_HAZARDPTRS_PER_THREAD 128

int hp_num_threads;
static size_t hp_scan_threshold;
hp_recl_t** hp_recls;
__thread hp_recl_t* t_recl_hp;


void hp_global_init(int num_threads) {
    hp_num_threads = num_threads;
    hp_scan_threshold = num_threads * MAX_HAZARDPTRS_PER_THREAD;
    
    hp_recls = (hp_recl_t**)calloc(num_threads, sizeof(hp_recl_t*));
    if (hp_recls == NULL) {
        fprintf(stderr, "Could not allocate HP data array\n");
        exit(EXIT_FAILURE);
    }
}

void hp_thread_init(int tid, size_t initial_bag_size) {
    assert(tid >= 0 && tid < hp_num_threads);
    assert(hp_recls != NULL);
    
    hp_recl_t* hp = (hp_recl_t*)l1_dcache_line_aligned_calloc(1, sizeof(hp_recl_t));
    if (hp == NULL) {
        fprintf(stderr, "Could not allocate HP data for thread %d\n", tid);
        exit(EXIT_FAILURE);
    }
    
    bag_init(&hp->connection_item_announcement, 64);
    bag_init(&hp->mem_announcements, 2);  // Max 2 simultaneous entries for non-items (hash table and clock)
    bag_init(&hp->retired_mem, NEXT_POW2(initial_bag_size));
    bag_init(&hp->retired_items, NEXT_POW2(initial_bag_size));
    bag_init_hashset(&hp->comparing, NEXT_POW2(2 * hp_num_threads * MAX_HAZARDPTRS_PER_THREAD));
    
    hp_recls[tid] = hp;
    t_recl_hp = hp;
}

size_t hp_reclaim_items(void) {
    size_t mem_freed = 0;

    bag_hashset_clear(&t_recl_hp->comparing);
    
    // Collect all hazard pointers from all threads (items only)
    for (int tid = 0; tid < hp_num_threads; tid++) {
        if (hp_recls[tid] == NULL) continue;
        
        hp_recl_t* other_hp = hp_recls[tid];
        for (size_t i = 0; i < other_hp->connection_item_announcement.size; i++) {
            bag_queue* conn_bq = (bag_queue*)other_hp->connection_item_announcement.elems[i];
            for (size_t j = conn_bq->start; j < conn_bq->b.size; j++) {
                bag_hashset_insert(&t_recl_hp->comparing, conn_bq->b.elems[j]);
            }
        }
    }
    
    // Scan retired bag and free objects not in hazard pointers
    for (size_t i = 0; i < t_recl_hp->retired_items.size; ) {
        void* obj = t_recl_hp->retired_items.elems[i];
        
        if (!bag_hashset_contains(&t_recl_hp->comparing, obj)) {
            slabs_free_batch_incremental(obj, &mem_freed);
            bag_remove(&t_recl_hp->retired_items, i);
            // Don't increment i since we removed an element
        } else {
            i++;
        }
    }
    
    if (mem_freed > 0)
        slabs_free_batch_finish(mem_freed);
    return mem_freed;
}

size_t hp_retire_item(void* ptr) {
    bag_put(&t_recl_hp->retired_items, ptr);

    if (can_reclaim() && t_recl_hp->retired_items.size >= hp_scan_threshold) {
        return hp_reclaim_items();
    }
    
    return 0;
}

size_t hp_reclaim_mem(void) {
    size_t reclaimed_objects = 0;

    bag_hashset_clear(&t_recl_hp->comparing);
    
    for (int tid = 0; tid < hp_num_threads; tid++) {
        if (hp_recls[tid] == NULL) continue;
        
        hp_recl_t* other_hp = hp_recls[tid];
        for (size_t i = 0; i < other_hp->mem_announcements.size; i++) {
            bag_hashset_insert(&t_recl_hp->comparing, other_hp->mem_announcements.elems[i]);
        }
    }
    
    for (size_t i = 0; i < t_recl_hp->retired_mem.size; ) {
        void* obj = t_recl_hp->retired_mem.elems[i];
        
        if (!bag_hashset_contains(&t_recl_hp->comparing, obj)) {
            free(obj);
            bag_remove(&t_recl_hp->retired_mem, i);
            reclaimed_objects++;
            // Don't increment i since we removed an element
        } else {
            i++;
        }
    }

    return reclaimed_objects;
}

size_t hp_retire_mem(void* ptr) {
    bag_put(&t_recl_hp->retired_mem, ptr);
    
    if (can_reclaim() && t_recl_hp->retired_mem.size >= hp_scan_threshold) {
        return hp_reclaim_mem();
    }

    return 0;
}

void hp_setup_connection_item_announcements(conn* c) {
    bag_queue* bq = &c->item_announcements;
    bag_init(&bq->b, NEXT_POW2(MAX_HAZARDPTRS_PER_THREAD));
    bq->start = 0;
    bag_put(&t_recl_hp->connection_item_announcement, bq);
}

size_t hp_do_system_item_reclamation(void) {
    for (size_t i = 0; i < hp_num_threads; i++) {
        hp_recls[i]->system_item_reclamation_active = true;
    }

    t_recl_hp->system_item_reclamation_active = false;

    // No need for can_reclaim() here since this will only be called if it's true
    if (t_recl_hp->retired_items.size > 0) {
        return hp_reclaim_items();
    }

    return 0;
}

void hp_check_system_item_reclamation(void) {
    if (can_reclaim() && t_recl_hp->system_item_reclamation_active) {
        t_recl_hp->system_item_reclamation_active = false;
        if (t_recl_hp->retired_items.size > 0) {
            hp_reclaim_items();
        }
    }
}

#else

__attribute__((unused)) static const int a;

#endif