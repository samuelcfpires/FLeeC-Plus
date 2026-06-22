#include "recl.h"

#if (defined(RECL_EBR) || defined(RECL_QSENSE)) && !defined(RECL_EBR_H)
#define RECL_EBR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "bag.h"

#define NUM_LIMBO_BAGS 3

// Epoch manipulation macros
#define EPOCH_MASK          (~(uint64_t)0x1)
#define QUIESCENT_MASK      ((uint64_t)0x1)
#define EPOCH_BITS(e)       ((e) & EPOCH_MASK)
#define IS_QUIESCENT(e)     ((e) & QUIESCENT_MASK)
#define SET_QUIESCENT(e)    ((e) | QUIESCENT_MASK)


// Per-thread reclamation structure
typedef struct reclamation {
    /*atomic*/ uint64_t announcement;
    int curr_bag_id;
    int checked;              // How far we've progressed in checking other threads
    int ops_since_check;      // Operations since last epoch advancement check
    size_t n_state_locks;
    bag limbo_bags[NUM_LIMBO_BAGS];
} reclamation;

extern int ebr_num_threads;
extern reclamation** ebr_recls;
extern __thread reclamation* t_recl_ebr;

void ebr_global_init(int num_threads);
void ebr_thread_init(int tid, size_t initial_bag_size);
void ebr_retire(void* item, int reclaim_type);
bool ebr_enter_quiescent(void);
void ebr_leave_quiescent(void);
// Used when memory is desesperately needed, or to force and monitor epoch advancement (for assoc/clock maintenance threads)
size_t ebr_force_announce_epoch(void);


/**
 * Ignores any quiescent state changes until ebr_unlock_state is called as many times as this was called.
 * 
 * This is used because in memcached.c's transmit function, the socket buffer may become full.
 * When this happens, the thread tells libevent to notify it when the socket is writable again so that it can resume transmission.
 * The thread is free to serve other connections while waiting for the socket to be writable.
 * However, since it will eventually need to access the items fetched from the initial get request that filled the socket buffer,
 * it can't enter quiescent until the pending transmission is finished, or else those items may be reclaimed, leading to a use-after-free.
 * 
 * There can be multiple pending transmissions at the same time for multiple clients,
 * so a counter is used to track how many pending transmissions there are.
 * Only after it reaches 0 can the thread re-enter quiescent state.
 */
static inline size_t ebr_lock_state(void) {
    return ++t_recl_ebr->n_state_locks;
}

static inline size_t ebr_unlock_state(void) {
    return --t_recl_ebr->n_state_locks;
}

static inline bool ebr_is_state_locked(void) {
    return t_recl_ebr->n_state_locks > 0;
}

#define CUSTOM_TYPE     1
#define OS_TYPE         2   // Reclaim to Operating System (normal free)

static inline void ebr_retire_item(void* item) {
	ebr_retire(item, CUSTOM_TYPE);
}
static inline void ebr_retire_mem(void* ptr) {
    ebr_retire(ptr, OS_TYPE);
}

#endif
