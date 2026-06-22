#include "recl.h"

#if !defined(RECL_QSENSE_H) && defined(RECL_QSENSE)
#define RECL_QSENSE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "ebr.h"
#include "hp.h"

#define QSENSE_EBR_SWITCH_MEM_THRESHOLD (2 << 30) // 1GB
#define QSENSE_HP_SWITCH_MEM_THRESHOLD (1 << 30) // 1GB

typedef enum {
    QSENSE_EBR = 0,
    QSENSE_HP = 1
} qsense_mode_e;

#define other_mode(m) (!(m))

typedef union {
    struct {
        qsense_mode_e mode;
        bool switching;
    };
    uint64_t data;
} qsense_state_t;

typedef struct qsense_recl {
    bool became_quiescent;
} qsense_recl_t;

extern int qsense_num_threads;

// do not access directly, use get_qsense_state()
extern qsense_state_t qsense_state;
extern __thread qsense_recl_t* t_recl_qsense;

#define get_qsense_state() __extension__({ \
    qsense_state_t state; \
    state.data = ALOAD(qsense_state.data); \
    state; \
})

#define is_qsense_switching() (get_qsense_state().switching)
#define get_qsense_mode() (get_qsense_state().mode)

void qsense_global_init(int num_threads);
void qsense_thread_init(int tid, size_t initial_bag_size);

void qsense_wait_for_system_quiescence(void);

void qsense_switch_mode(qsense_mode_e new_mode);

/*
 * The mode choice in these functions may seem confusing, but it's simple:
 * - During normal operation, just use the current mode;
 * - During a switch:
 *  + For retiring, always use the new mode;
 *    * This is becase records retired during a switch may be being accessed by threads in both modes
 *      Since we can't perform safe-to-free detection for both modes at the same time, we wait for the old threads to become quiescent (stop accessing any shared records),
 *      and now only threads in the new mode may be accessing them, so we can safely reclaim them using the new mode's mechanism
 *  + For anything else, keep using the old mode until we become quiescent
 *    * This is because if the switch happens during a thread's operation, it must finish that operation in the mode that it started with
 * 
 * Threads may only switch modes during a switch when they have no suspended connections, otherwise they aren't truly quiescent and may still be accessing records
 */


static inline void qsense_retire_item(void* item) {
    switch(get_qsense_mode()) {
        case QSENSE_EBR: ebr_retire_item(item); break;
        case QSENSE_HP: hp_retire_item(item); break;
    }
}

static inline void qsense_retire_mem(void* ptr) {
    switch(get_qsense_mode()) {
        case QSENSE_EBR: ebr_retire_mem(ptr); break;
        case QSENSE_HP: hp_retire_mem(ptr); break;
    }
}

/**
 * Returns the mode that should be used for protecting records at the current moment for the calling thread.
 */
static inline qsense_mode_e qsense_get_protection_mode(void) {
    qsense_state_t curr_state = get_qsense_state();
    qsense_mode_e mode = curr_state.mode;

    if (curr_state.switching && !ALOAD(t_recl_qsense->became_quiescent)) {
        // Keep protecting records/operating in old mode during switch until we become quiescent
        return other_mode(mode);
    }

    return mode;
}

/* EBR stuff */

// Must always maintain the quiescent state updated for switching modes
// The qsense management thread must know which threads are quiescent, even in HP
// otherwise a quiescent thread that is asleep (possibly due to lack of requests) will block the switch indefinitly
static inline void qsense_enter_quiescent(void) {
    ebr_enter_quiescent();

    switch(qsense_get_protection_mode()) {
        case QSENSE_HP: hp_check_system_item_reclamation(); break;
        default: break;
    }

    if (!ALOAD(t_recl_qsense->became_quiescent) && !ebr_is_state_locked()) {
        ASTORE(t_recl_qsense->became_quiescent, true);
    }
}

#define qsense_leave_quiescent ebr_leave_quiescent

static inline bool qsense_is_state_locked(void) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_EBR: return ebr_is_state_locked();
        default: return false;
    }
}

// Must always maintain number of suspended connections because threads can only switch between modes when there are none
#define qsense_lock_state   ebr_lock_state
#define qsense_unlock_state ebr_unlock_state

/* HP stuff */

static inline void qsense_protect_push_item(void* ptr) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: hp_protect_push_item(ptr); break;
        default: break;
    }
}

static inline void* qsense_protect_push_load_mem(void* ptr, int order) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: return hp_protect_push_load_mem(ptr, order);
        default: return __atomic_load_n(&(ptr), order);
    }
}

#define qsense_protect_push_load_mem2(ret1, ptr1, ret2, ptr2, order) do { \
    switch (qsense_get_protection_mode()) { \
        case QSENSE_HP: hp_protect_push_load_mem2(ret1, ptr1, ret2, ptr2, order); break; \
        default: \
            ret1 = __atomic_load_n(&(ptr1), order); \
            ret2 = __atomic_load_n(&(ptr2), order); \
            break; \
    } \
} while(0)

static inline void qsense_unprotect_pop_item(void) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: hp_unprotect_pop_item(); break;
        default: break;
    }
}

static inline void qsense_unprotect_pop_mem(void) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: hp_unprotect_pop_mem(); break;
        default: break;
    }
}

#define qsense_unprotect_all_items(c) do { \
    switch (qsense_get_protection_mode()) { \
        case QSENSE_HP: hp_unprotect_all_items(c); break; \
        default: break; \
    } \
} while(0)

static inline void qsense_unprotect_all_mem(void) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: hp_unprotect_all_mem(); break;
        default: break;
    }
}

#define qsense_unprotect_dequeue_items(c, amount) do { \
    switch (qsense_get_protection_mode()) { \
        case QSENSE_HP: hp_unprotect_dequeue_items(c, amount); break; \
        default: break; \
    } \
} while(0)

#define qsense_setup_connection_item_announcements hp_setup_connection_item_announcements

// No need to set in EBR mode because threads only switch the protection mode between operations (and thus connections)
#define qsense_set_curr_item_announcements_connection(c) do { \
    switch (qsense_get_protection_mode()) { \
        case QSENSE_HP: hp_set_curr_item_announcements_connection(c); break; \
        default: break; \
    } \
} while(0)

static inline void qsense_check_system_item_reclamation(void) {
    switch (qsense_get_protection_mode()) {
        case QSENSE_HP: hp_check_system_item_reclamation(); break;
        default: break;
    }
}

#endif
