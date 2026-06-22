#include "recl.h"

#if (defined(RECL_HP) || defined(RECL_QSENSE)) && !defined(RECL_HP_H)
#define	RECL_HP_H

#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include "memcached.h"
#include "bag.h"

typedef struct conn conn;

typedef struct hp_recl {
    bag connection_item_announcement;   // Item announcement for each connection
    bag* curr_item_announcements;
    bag mem_announcements;       // Stack of announced hazard pointers for memory blocks (max 3 entries)
    bag retired_mem, retired_items;                 // Bag of retired objects waiting to be freed
    bag comparing;               // Hash set (using bag) for comparing during retirement scan
    bool system_item_reclamation_active;
} hp_recl_t;

extern int hp_num_threads;
extern hp_recl_t** hp_recls;
extern __thread hp_recl_t* t_recl_hp;

void hp_global_init(int num_threads);
void hp_thread_init(int tid, size_t initial_bag_size);

size_t hp_retire_item(void* ptr);
size_t hp_retire_mem(void* ptr);
size_t hp_reclaim_items(void);
size_t hp_reclaim_mem(void);

size_t hp_do_system_item_reclamation(void);
void hp_setup_connection_item_announcements(conn* c);
void hp_check_system_item_reclamation(void);

#define hp_set_curr_item_announcements_connection(c) \
    t_recl_hp->curr_item_announcements = &((c)->item_announcements.b);

#define _hp_protect_push_no_membar(ptr, bag_ptr) do { \
    bag* _target = (bag*)(bag_ptr); \
    bag_put(_target, ptr); \
} while(0)

#define _hp_protect_push(ptr, bag_ptr) do { \
    _hp_protect_push_no_membar(ptr, bag_ptr); \
    __sync_synchronize(); \
} while(0)

#define _hp_unprotect_pop(bag_ptr) do { \
    bag* _target = (bag*)(bag_ptr); \
    _target->size--; \
} while(0)

#define _hp_unprotect_all(bag_ptr) do { \
    bag* _target = (bag*)(bag_ptr); \
    _target->size = 0; \
} while(0)

// Macros for item vs mem operations
#define hp_protect_push_mem(ptr)            _hp_protect_push(ptr, &t_recl_hp->mem_announcements)
#define hp_protect_push_mem_no_membar(ptr)  _hp_protect_push_no_membar(ptr, &t_recl_hp->mem_announcements)
#define hp_unprotect_pop_mem()              _hp_unprotect_pop(&t_recl_hp->mem_announcements)
#define hp_unprotect_all_mem()              _hp_unprotect_all(&t_recl_hp->mem_announcements)
#define hp_protect_push_item(ptr)           _hp_protect_push(ptr, t_recl_hp->curr_item_announcements)
#define hp_unprotect_pop_item()             _hp_unprotect_pop(t_recl_hp->curr_item_announcements)

// This is a micro-optimization to unprotect items as they are sent to the client during GET requests
// but hp_unprotect_all_items must always be called at the end of the request
#define hp_unprotect_dequeue_items(c, amount) (c->item_announcements.start += (amount))
#define hp_unprotect_all_items(c) do { \
    bag_queue* bq = &(c->item_announcements); \
    bq->b.size = bq->start = 0; \
} while(0)


#define hp_protect_push_load_mem(ptr, order) __extension__ ({  \
    typeof(ptr) ptr_val; \
    typeof(&(ptr)) ptr_addr = &(ptr);                          \
    do { \
        ptr_val = __atomic_load_n(ptr_addr, order);         \
        hp_protect_push_mem(ptr_val);                  \
        if (ptr_val != __atomic_load_n(ptr_addr, __ATOMIC_RELAXED)) { \
            hp_unprotect_pop_mem(); \
        } else { \
            break; \
        } \
    } while(true);          \
    ptr_val; \
})

#define hp_protect_push_load_mem2(ret1, ptr1, ret2, ptr2, order) __extension__ ({  \
    typeof(ptr1) ptr1_val; \
    typeof(ptr2) ptr2_val; \
    typeof(&(ptr1)) ptr1_addr = &(ptr1);                          \
    typeof(&(ptr2)) ptr2_addr = &(ptr2);                          \
    do { \
        ptr1_val = __atomic_load_n(ptr1_addr, order);         \
        ptr2_val = __atomic_load_n(ptr2_addr, order);         \
        hp_protect_push_mem_no_membar(ptr1_val);                  \
        hp_protect_push_mem(ptr2_val);                  \
        if (ptr1_val != __atomic_load_n(ptr1_addr, __ATOMIC_RELAXED) || \
            ptr2_val != __atomic_load_n(ptr2_addr, __ATOMIC_RELAXED)) { \
            hp_unprotect_pop_mem(); \
            hp_unprotect_pop_mem(); \
        } else { \
            break; \
        } \
    } while(true);          \
    ret1 = ptr1_val; \
    ret2 = ptr2_val; \
})


#endif /* RECL_HP_H */