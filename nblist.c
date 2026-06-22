#ifdef USE_ASSOC_NBLIST

#include "nblist.h"
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "recl.h"

#ifndef RECL_EBR
#error "nblist incompatible with non-EBR reclamation."
#endif


#ifdef NHOPS
size_t** nhops_real;
size_t** nhops_fake;
size_t** nhops_marked_real;
size_t** nhops_marked_fake;
extern __thread int g_tid;
size_t* t_nhops;
size_t* t_nhops_marked;
#endif


/* Compare and Swap macro */
#define CAS(p, e, d) __atomic_compare_exchange_n(p, e, d, \
    0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)


//Must only be called sequentially
void free_list(List* l) {
    item *n = l->head->next, *next = NULL;
    while(n != l->tail && n != NULL) {
        next = (item*) get_unmarked_reference(n->next);
        free(n);
        n = next;
    }
    free(l->head);
    free(l->tail);
    free(l);
}

void print_list(List * list) {
    item* n = list->head;
    item* next = list->head->next;
    item* keep_real = next;
    int c = 0;
    while(next != list->tail) {
        n = next;
        keep_real = n->next;
        printf("%.*s:%ld:%d ",
            n->nkey, ITEM_key(n), is_marked_reference(keep_real),
            is_marked_replacement_reference(keep_real) > 0);
        c++;
        next = (item *) get_unmarked_reference(n->next);
    }
    printf("\n");
}


//Check if alignement of these 2 elements in 2 structs is the same
bool check_alignment() {
    item* it = (item*) malloc(sizeof(item));
    fake_item* fake_it = (fake_item*) malloc(sizeof(fake_item));
    uint8_t next_alignment = (long) &(it->next) - (long) it; 
    uint8_t fake_next_alignment = (long) &(fake_it->next) - (long) fake_it; 
    free(it);
    free(fake_it);
    return (next_alignment == fake_next_alignment);
}



List* new_nblist(void) {
#ifdef NHOPS
    if (!nhops_real) {
        nhops_real = (size_t**) malloc(sizeof(size_t*) * settings.num_threads);
        nhops_fake = (size_t**) malloc(sizeof(size_t*) * settings.num_threads);
        nhops_marked_real = (size_t**) malloc(sizeof(size_t*) * settings.num_threads);
        nhops_marked_fake = (size_t**) malloc(sizeof(size_t*) * settings.num_threads);
        for (int i = 0; i < settings.num_threads; i++) {
            nhops_real[i] = (size_t*) calloc(NHOPS_SIZE, sizeof(size_t));
            nhops_fake[i] = (size_t*) calloc(NHOPS_SIZE, sizeof(size_t));
            nhops_marked_real[i] = (size_t*) calloc(NHOPS_SIZE, sizeof(size_t));
            nhops_marked_fake[i] = (size_t*) calloc(NHOPS_SIZE, sizeof(size_t));
            t_nhops = nhops_real[i];
            t_nhops_marked = nhops_marked_real[i];
        }
    }
#endif

    if(!check_alignment()) {
        fprintf(stderr, "Alignment of struct item and struct fake_item differs!");
        exit(EXIT_FAILURE);
    }
	List *l = (List*) malloc(sizeof(List));
	fake_item *head = (fake_item*) malloc(sizeof(fake_item));
	fake_item *tail = (fake_item*) malloc(sizeof(fake_item));
	l->head = (item*) head;
	l->tail = (item*) tail;
	head->next = l->tail;
	return l;
}


item* search(List* list, const char* search_key, const size_t nkey, item **left_item) {
	//NULL because of warnings
	item *left_item_next = NULL, *right_item;
#ifdef NHOPS
    size_t max_hop_count = 0;
    size_t max_marked_count = 0;
#endif

search_again:;
#ifdef NHOPS
    size_t hop_count = 0;
    size_t marked_count = 0;
#endif
	do {
        item *t = list->head;
        item *t_next = list->head->next; 
        int marked_counter = 0;
#ifdef NHOPS
    hop_count++;
#endif

		/* 1: Find left_item and right_item */
        do {
            if (!is_marked_reference(t_next)) {
                (* left_item) = t;
                left_item_next = t_next;
                marked_counter = 0;
            } else {
                marked_counter++;
#ifdef NHOPS
                marked_count++;
#endif
            }

            t = (item *) get_unmarked_reference(t_next);
            if (t == list->tail)
				break;
            t_next = t->next;
#ifdef NHOPS
            hop_count++;
#endif
        } while (is_marked_reference(t_next) ||
            //Compare keys
            (KEY_cmp(ITEM_key(t), search_key, t->nkey, nkey) < 0)); /*B1*/

        right_item = t; 
		/* 2: Check items are adjacent */
        if (left_item_next == right_item) {
            if ((right_item != list->tail) && is_marked_reference(right_item->next)) {
#ifdef NHOPS
                if (right_item != list->tail)
                    hop_count++;
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
#endif
                goto search_again; /*G1*/
			} else {
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
                t_nhops[(max_hop_count > NHOPS_SIZE ? NHOPS_SIZE : max_hop_count) - 1]++;
                t_nhops_marked[(max_marked_count > NHOPS_SIZE ? NHOPS_SIZE : max_marked_count) - 1]++;
#endif
				return right_item; /*R1*/
			}
		}

 		/* 3: Remove one or more marked items */
        if (CAS(&((*left_item)->next), &left_item_next, right_item)) { /*C1*/
            //Add one or more marked items to be reclaimed
            item *e = (item*) get_unmarked_reference(left_item_next);
            while(e != NULL && marked_counter > 0) {
                recl_retire_item(e);
                assert(is_marked_reference(e->next));
                e = (item*) get_unmarked_reference(e->next);
//#ifdef NHOPS
//                hop_count++;
//#endif
                marked_counter--;
            }

            if ((right_item != list->tail) && is_marked_reference(right_item->next)) {
//#ifdef NHOPS
//                if (right_item != list->tail)
//                    hop_count++;
//#endif
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
#endif
				goto search_again; /*G2*/
            } else {
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
                t_nhops[(max_hop_count > NHOPS_SIZE ? NHOPS_SIZE : max_hop_count) - 1]++;
                t_nhops_marked[(max_marked_count > NHOPS_SIZE ? NHOPS_SIZE : max_marked_count) - 1]++;
#endif
		      	return right_item; /*R2*/
            }
		}

    } while (true); /*B2*/
}


int cleanup(List* list, size_t *freed_bytes) {
	item *left_item_next, *right_item, *left_item;
	left_item_next = right_item = left_item = NULL;
    int items_removed;
    int total_items_removed = 0;
    size_t total_freed_bytes = 0;

    do {
        item * t = list->head;
        item * t_next = list->head->next; 

        items_removed = 0;
continue_cleanup:
        /* Traverse until we find a marked item */
        do {
            if (!is_marked_reference(t_next)) {
                left_item = t;
                left_item_next = t_next;
            }
            t = (item *) get_unmarked_reference(t_next);
            if (t == list->tail) {
                if (freed_bytes) *freed_bytes = total_freed_bytes;
				return total_items_removed; /* Did not find marked items */
            }
            t_next = t->next;
        } while (!is_marked_reference(t_next));

		/* 1: Find left_item and right_item */
        while (is_marked_reference(t_next)) {
            items_removed++;
            t = (item *) get_unmarked_reference(t_next);
            if (t == list->tail)
				break;
            t_next = t->next;
        }

        right_item = t; 

 		/* 3: Remove one or more marked items */
        if (CAS(&(left_item->next), &left_item_next, right_item)) {
            item *e = (item*) get_unmarked_reference(left_item_next);
            total_items_removed += items_removed;
            while(e != NULL && items_removed > 0) {
                total_freed_bytes += ITEM_ntotal(e);
                recl_retire_item(e);
                assert(is_marked_reference(e->next));
                e = (item*) get_unmarked_reference(e->next);
                items_removed--;
            }
            goto continue_cleanup;
		} /* else { retry; }*/

    } while (true); /*B2*/
}

//Mark every node in list as logically deleted
int __mark_all_nodes(List* list) {
    item *tail, *e, *e_next;
    e = list->head->next;
    tail = list->tail;
    int marked_nodes = 0;

	while (e != tail) {
        do {
            e_next = e->next;

            if (is_marked_reference(e_next) ||
                CAS(&(e->next), &e_next, (item*) get_marked_reference(e_next)))
		    		break;

        } while(true);

        marked_nodes++;
        e = (item*) get_unmarked_reference(e->next);
    }

    return marked_nodes;
}

//Wrapper for mix of routines that empty a list
int empty_list(List* list) {
    __mark_all_nodes(list);
    size_t freed_bytes;
    return cleanup(list, &freed_bytes);
}

bool is_empty(List *list) {
    return list->head->next == list->tail;
}

bool insert(List *list, item *it) {
    item *right_item, *left_item;

    do {
        right_item = search(list, ITEM_key(it), it->nkey, &left_item);
        if ((right_item == NULL) ||
            ((right_item != list->tail) && (ITEM_cmp(right_item, it) == 0))) /*T1*/
			return false;

        it->next = right_item;

        if (CAS(&(left_item->next), &right_item, it)) /*C2*/
            return true;

    } while (true); /*B3*/
}

item* del(List* list, const char* search_key, const size_t nkey, bool reclaim, bool *found) {
    item *right_item, *right_item_next, *left_item = NULL;

    do {
        right_item = search(list, search_key, nkey, &left_item);
        if ((right_item == NULL) || (right_item == list->tail) ||
            (KEY_cmp(ITEM_key(right_item), search_key, right_item->nkey, nkey) != 0)) /*T1*/
            return NULL;

        right_item_next = right_item->next;

        if (!is_marked_reference(right_item_next))
            if (CAS(&(right_item->next), /*C3*/ &right_item_next,
					(item *) get_marked_reference(right_item_next)))
				break;

    } while (true); /*B4*/

    *found = true;

    if (!CAS(&(left_item->next), &right_item, right_item_next)) {/*C4*/
        right_item = (item*) get_unmarked_reference(right_item);
        right_item = search(list, ITEM_key(right_item), right_item->nkey, &left_item);
        return NULL;
    }

    /* add removed item to be reclaimed */
    if(reclaim)
        recl_retire_item(right_item);

    return right_item;
}


item* replace(List* list, const char* search_key, const size_t nkey, item *new_it,
    bool reclaim, bool *inserted) {

    item *right_item, *left_item = NULL;
	*inserted = false;

    do {
        right_item = search_last(list, search_key, nkey, &left_item);

        new_it->next = right_item;

		if(left_item == NULL || left_item->next == NULL)
			return NULL;

        if (CAS(&(left_item->next), &right_item, new_it)) {
			*inserted = true;
			break;
		}

    } while (true); /*B3*/

	bool found;
	item* ret = del(list, search_key, nkey, reclaim, &found);
    return ret;
}

//Simple search, with slight difference of searching
//	until current key is not greater than searched key
//
//	result: left and right item find last occurrence of a given key in a list
item* search_last(List* list, const char* search_key, const size_t nkey, item **left_item) {
	//NULL because of warnings
	item *left_item_next = NULL, *right_item;
#ifdef NHOPS
    size_t max_hop_count = 0;
    size_t max_marked_count = 0;
#endif

search_again:;
#ifdef NHOPS
    size_t hop_count = 0;
    size_t marked_count = 0;
#endif
	do {
        item *t = list->head;
        item *t_next = list->head->next; 
        int marked_counter = 0;
#ifdef NHOPS
    hop_count++;
#endif

		//Wether the last item traversal had the same that we are looking for
		bool last_item_equal = true;

		/* 1: Find left_item and right_item */
        do {
            if (!is_marked_reference(t_next)) {
                (* left_item) = t;
                left_item_next = t_next;
                marked_counter = 0;
            } else {
                marked_counter++;
#ifdef NHOPS
                marked_count++;
#endif
			}

            t = (item *) get_unmarked_reference(t_next);
            if (t == list->tail)
				break;

            t_next = t->next;
#ifdef NHOPS
            hop_count++;
#endif

        } while (is_marked_reference(t_next) ||
            //Compare keys
            ((last_item_equal = (KEY_cmp(ITEM_key(t), search_key, t->nkey, nkey) <= 0)))); /*B1*/


        right_item = t; 

		/* 2: Check items are adjacent */
        if (left_item_next == right_item) {

            if ((right_item != list->tail) && is_marked_reference(right_item->next)) {
#ifdef NHOPS
                if (right_item != list->tail)
                    hop_count++;
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
#endif
                goto search_again; /*G1*/
			} else {
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
                t_nhops[(max_hop_count > NHOPS_SIZE ? NHOPS_SIZE : max_hop_count) - 1]++;
                t_nhops_marked[(max_marked_count > NHOPS_SIZE ? NHOPS_SIZE : max_marked_count) - 1]++;
#endif
				return right_item; /*R1*/
			}
		}

 		/* 3: Remove one or more marked items */
        if (CAS(&((*left_item)->next), &left_item_next, right_item)) { /*C1*/
            //Add one or more marked items to be reclaimed
            item *e = (item*) get_unmarked_reference(left_item_next);
            while(e != NULL && marked_counter > 0) {
                recl_retire_item(e);
                assert(is_marked_reference(e->next));
                e = (item*) get_unmarked_reference(e->next);
//#ifdef NHOPS
//                hop_count++;
//#endif
                marked_counter--;
            }

            if ((right_item != list->tail) && is_marked_reference(right_item->next)) {
//#ifdef NHOPS
//                if (right_item != list->tail)
//                    hop_count++;
//#endif
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
#endif
				goto search_again; /*G2*/
            } else {
#ifdef NHOPS
                if (hop_count > max_hop_count) {
                    max_hop_count = hop_count;
                    max_marked_count = marked_count;
                }
                t_nhops[(max_hop_count > NHOPS_SIZE ? NHOPS_SIZE : max_hop_count) - 1]++;
                t_nhops_marked[(max_marked_count > NHOPS_SIZE ? NHOPS_SIZE : max_marked_count) - 1]++;
#endif
		      	return right_item; /*R2*/
			}
		}

    } while (true); /*B2*/
}


item* get(List *list, const char* search_key, const size_t nkey) {
    item *right_item, *left_item = NULL;
    right_item = search(list, search_key, nkey, &left_item);
    if ((right_item == NULL) || (right_item == list->tail) ||
        (KEY_cmp(ITEM_key(right_item), search_key, right_item->nkey, nkey) != 0)) {
        return NULL;
    } else {
		return right_item;
    }
}

#else

__attribute__((unused)) static const char nblist_empty_file_warning;

#endif