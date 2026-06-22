#ifndef RECL_H
#define	RECL_H

#if !defined(RECL_HP) && !defined(RECL_EBR) && !defined(RECL_QSENSE)
#define RECL_QSENSE
#endif


#define OOM_RECL_NTRIES_EBR 100
#define OOM_RECL_USLEEP_EBR 50
#define OOM_RECL_NTRIES_HP 10
#define OOM_RECL_USLEEP_HP 50

#ifdef RECL_EBR
#define OOM_RECL_NTRIES OOM_RECL_NTRIES_EBR
#define OOM_RECL_USLEEP OOM_RECL_USLEEP_EBR
#else /* RECL_HP */
#define OOM_RECL_NTRIES OOM_RECL_NTRIES_HP
#define OOM_RECL_USLEEP OOM_RECL_USLEEP_HP
#endif


#ifdef RECL_EBR
#include "ebr.h"

#define recl_num_threads ebr_num_threads

#define recl_global_init            ebr_global_init
#define recl_thread_init            ebr_thread_init
#define recl_retire_item            ebr_retire_item
#define recl_retire_mem             ebr_retire_mem
#define recl_enter_quiescent        ebr_enter_quiescent
#define recl_leave_quiescent        ebr_leave_quiescent
#define recl_force_announce_epoch   ebr_force_announce_epoch
#define recl_is_state_locked        ebr_is_state_locked
#define recl_lock_state             ebr_lock_state
#define recl_unlock_state           ebr_unlock_state

#define recl_protect_push_item(it)  do {} while(0)
#define recl_unprotect_pop_item()   do {} while(0)
#define recl_unprotect_pop_mem()    do {} while(0)
#define recl_unprotect_all_items(c) do {} while(0)
#define recl_unprotect_all_mem()    do {} while(0)
#define recl_unprotect_dequeue_items(c, amount)   do {} while(0)
#define recl_setup_connection_item_announcements(c)  do {} while(0)
#define recl_set_curr_item_announcements_connection(c)  do {} while(0)
#define recl_check_system_item_reclamation() do {} while(0)

#define recl_protect_push_load_mem(ptr, order) __atomic_load_n(&(ptr), order)
#define recl_protect_push_load_mem2(ret1, ptr1, ret2, ptr2, order) do { \
    ret1 = __atomic_load_n(&(ptr1), order); \
    ret2 = __atomic_load_n(&(ptr2), order); \
} while(0)

#define recl_advise_normal_operation()  do {} while(0)
#define recl_advise_mem_pressure()      do {} while(0)

#elif defined(RECL_HP)
#include "hp.h"

#define recl_num_threads hp_num_threads

#define recl_global_init            hp_global_init
#define recl_thread_init            hp_thread_init
#define recl_retire_item            hp_retire_item
#define recl_retire_mem             hp_retire_mem

#define recl_protect_push_item      hp_protect_push_item
#define recl_protect_push_load_mem  hp_protect_push_load_mem
#define recl_protect_push_load_mem2 hp_protect_push_load_mem2
#define recl_unprotect_pop_item     hp_unprotect_pop_item
#define recl_unprotect_pop_mem      hp_unprotect_pop_mem
#define recl_unprotect_all_items    hp_unprotect_all_items
#define recl_unprotect_all_mem      hp_unprotect_all_mem
#define recl_unprotect_dequeue_items   hp_unprotect_dequeue_items

#define recl_setup_connection_item_announcements    hp_setup_connection_item_announcements
#define recl_set_curr_item_announcements_connection hp_set_curr_item_announcements_connection
#define recl_check_system_item_reclamation          hp_check_system_item_reclamation

// Check if another thread needs memory after finishing an operation
#define recl_enter_quiescent        hp_check_system_item_reclamation
#define recl_leave_quiescent()      do {} while(0)
#define recl_force_announce_epoch() (0)
#define recl_is_state_locked()      (false)
#define recl_lock_state()           __extension__ ({ 0; })
#define recl_unlock_state()         __extension__ ({ 0; })

#define recl_advise_normal_operation()  do {} while(0)
#define recl_advise_mem_pressure()      do {} while(0)


#elif defined(RECL_QSENSE)
#include "qsense.h"

#define recl_num_threads qsense_num_threads

#define recl_global_init            qsense_global_init
#define recl_thread_init            qsense_thread_init
#define recl_retire_item            qsense_retire_item
#define recl_retire_mem             qsense_retire_mem
#define recl_enter_quiescent        qsense_enter_quiescent
#define recl_leave_quiescent        qsense_leave_quiescent
#define recl_is_state_locked        qsense_is_state_locked
#define recl_lock_state             qsense_lock_state
#define recl_unlock_state           qsense_unlock_state

#define recl_protect_push_item      qsense_protect_push_item
#define recl_protect_push_load_mem  qsense_protect_push_load_mem
#define recl_protect_push_load_mem2 qsense_protect_push_load_mem2
#define recl_unprotect_pop_item     qsense_unprotect_pop_item
#define recl_unprotect_pop_mem      qsense_unprotect_pop_mem
#define recl_unprotect_all_items    qsense_unprotect_all_items
#define recl_unprotect_all_mem      qsense_unprotect_all_mem
#define recl_unprotect_dequeue_items   qsense_unprotect_dequeue_items

#define recl_setup_connection_item_announcements    qsense_setup_connection_item_announcements
#define recl_set_curr_item_announcements_connection qsense_set_curr_item_announcements_connection
#define recl_check_system_item_reclamation          qsense_check_system_item_reclamation

#define recl_advise_normal_operation()  qsense_switch_mode(QSENSE_EBR)
#define recl_advise_mem_pressure()      qsense_switch_mode(QSENSE_HP)


#else
#error "No reclamation scheme defined. Define RECL_EBR or RECL_HP."
#endif

#endif