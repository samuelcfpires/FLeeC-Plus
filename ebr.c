#include "recl.h"

#if defined(RECL_EBR) || defined(RECL_QSENSE)

#include "memcached.h"

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

#ifdef RECL_QSENSE
// Only allow reclamation if currently in EBR mode (not switching), or during a switch to HP mode 
#define can_reclaim() __extension__ ({ \
    qsense_state_t curr_state = get_qsense_state(); \
    ((!curr_state.switching && curr_state.mode == QSENSE_EBR) || (curr_state.switching && curr_state.mode == QSENSE_HP)); \
})
#else
#define can_reclaim() (true)
#endif

#define EPOCH_INCREMENT 2
#define STARTING_EPOCH  0
#define DEFAULT_MIN_OPS_BEFORE_EPOCH_CHECK 128

#define get_os_marked_reference(x)  (((uintptr_t)(x)) | 1)
#define is_os_marked_reference(x)   (((uintptr_t)(x)) & 1)
#define get_unmarked_reference(x)   (((uintptr_t)(x)) & (~(uintptr_t)1))

static /*atomic*/ uint64_t ebr_curr_epoch;
reclamation** ebr_recls;
int ebr_num_threads;
__thread reclamation* t_recl_ebr;

static int min_ops_before_epoch_check = DEFAULT_MIN_OPS_BEFORE_EPOCH_CHECK;

// Initialize global EBR coordinator
void ebr_global_init(int num_threads) {
    ebr_curr_epoch = STARTING_EPOCH;
    ebr_recls = malloc(num_threads * sizeof(reclamation*));

    for (int i = 0; i < num_threads; ++i) {
        reclamation* recl = l1_dcache_line_aligned_alloc(sizeof(reclamation));
        recl->announcement = SET_QUIESCENT(STARTING_EPOCH);
        recl->curr_bag_id = STARTING_EPOCH / EPOCH_INCREMENT % NUM_LIMBO_BAGS;
        recl->checked = 0;
        recl->ops_since_check = 0;
        recl->n_state_locks = 0;
        ebr_recls[i] = recl;
    }

    ebr_num_threads = num_threads;
}

// Initialize per-thread reclamation structure
void ebr_thread_init(int tid, size_t initial_bag_size) {
    t_recl_ebr = ebr_recls[tid];
    for(int i = 0; i < NUM_LIMBO_BAGS; i++) {
        bag_init(&t_recl_ebr->limbo_bags[i], initial_bag_size);
    }
}

// Clear a limbo bag and reclaim all items
size_t ebr_clear_bag(bag* b);
size_t ebr_clear_bag(bag* b) {
    size_t mem_freed = 0;

    for (int i = 0; i < b->size; i++) {
        if (is_os_marked_reference(b->elems[i])) {
            free((void*)get_unmarked_reference(b->elems[i]));
        } else {
            slabs_free_batch_incremental((void*)b->elems[i], &mem_freed);
        }
    }

    if (mem_freed > 0)
        slabs_free_batch_finish(mem_freed);

    b->size = 0;

    return mem_freed;
}

static void rotate_epoch_bags(reclamation* recl) {
    int freeable_id = (recl->curr_bag_id + 1) % NUM_LIMBO_BAGS;
    ebr_clear_bag(&recl->limbo_bags[freeable_id]);
    recl->curr_bag_id = freeable_id;
}

// Add a retired item to the appropriate epoch bag
void ebr_retire(void* item, int reclaim_type) {
    void* marked_item = NULL;
    bag* curr_bag = &t_recl_ebr->limbo_bags[t_recl_ebr->curr_bag_id];

    switch(reclaim_type) {
        case CUSTOM_TYPE:
            marked_item = (void*) item;
            break;
        case OS_TYPE:
            marked_item = (void*) get_os_marked_reference(item);
            break;
    }

    bag_put(curr_bag, marked_item);
}

// Enter quiescent state (not accessing shared data structures)
bool ebr_enter_quiescent(void) {
    if (ebr_is_state_locked()) {
        return false;
    }

    ASTORE(t_recl_ebr->announcement, SET_QUIESCENT(t_recl_ebr->announcement));
    return true;
}

// Leave quiescent state (start accessing shared data structures)
void ebr_leave_quiescent(void) {
    if (ebr_is_state_locked()) {
        return;
    }

    asm volatile("": : :"memory");
    uint64_t curr_epoch = ALOAD(ebr_curr_epoch);
    if (!can_reclaim()) {
        goto skip;
    }

    uint64_t local_epoch = EPOCH_BITS(t_recl_ebr->announcement);

    // If we're entering a new epoch, rotate bags and reclaim old items
    if (local_epoch < curr_epoch) {
        rotate_epoch_bags(t_recl_ebr);
        t_recl_ebr->checked = 0;
    }

    // Periodically check if we can advance the epoch (NUMA-friendly incremental scan)
    if ((++t_recl_ebr->ops_since_check % min_ops_before_epoch_check) == 0) {
        int other_tid = t_recl_ebr->checked;
        if (other_tid < ebr_num_threads) {
            uint64_t other_ann = ALOAD(ebr_recls[other_tid]->announcement);
            if (EPOCH_BITS(other_ann) == curr_epoch || IS_QUIESCENT(other_ann)) {
                t_recl_ebr->checked++;
                if (t_recl_ebr->checked >= ebr_num_threads) {
                    // All threads are in current epoch or quiescent, try to advance
                    __sync_bool_compare_and_swap(&ebr_curr_epoch, curr_epoch, curr_epoch + EPOCH_INCREMENT);
                }
            }
        }
    }

skip:
    // Announce we're now active in the current epoch (non-quiescent)
    asm volatile("": : :"memory");
    ASTORE(t_recl_ebr->announcement, curr_epoch);
}

size_t ebr_force_announce_epoch(void) {
    if (ebr_is_state_locked()) {
        return 0;
    }

    asm volatile("": : :"memory");
    size_t bags_freed = 0;
    uint64_t curr_epoch = ALOAD(ebr_curr_epoch);
    // No need to check for can_reclaim() here since this will only be called if it's true

    uint64_t local_epoch = EPOCH_BITS(t_recl_ebr->announcement);

    for (int i = 0; i < ebr_num_threads; i++) {
        uint64_t ann = ALOAD(ebr_recls[i]->announcement);
        if (EPOCH_BITS(ann) < curr_epoch && !IS_QUIESCENT(ann)) {
            goto abort;
        }
    }

    __sync_bool_compare_and_swap(&ebr_curr_epoch, curr_epoch, curr_epoch + EPOCH_INCREMENT);
    curr_epoch += EPOCH_INCREMENT;

abort:
    if (local_epoch < curr_epoch) {
        rotate_epoch_bags(t_recl_ebr);
        bags_freed++;
    }

    asm volatile("": : :"memory");
    ASTORE(t_recl_ebr->announcement, SET_QUIESCENT(curr_epoch));
    return bags_freed;
}

#else

__attribute__((unused)) static const int a;

#endif