#ifndef NBLIST_H
#define NBLIST_H

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include "memcached.h"
#include "ebr.h"


#ifdef NHOPS
#define NHOPS_SIZE 256
#endif


#define KEY_cmp(key1, key2, size1, size2) __extension__({int32_t diff = (size1 - size2); \
    diff != 0 ? diff : \
    (strncmp(key1, key2, size1));})

#define ITEM_cmp(it1, it2) KEY_cmp(ITEM_key(it1), ITEM_key(it2), it1->nkey, it2->nkey)

/* Declarations */
typedef struct List {
	item *head;
	item *tail;
} List;


void free_list(List* l);
void print_list(List * list);

bool check_alignment(void);

List* new_nblist(void);
int cleanup(List* list, size_t *freed_bytes);
int empty_list(List* list);
bool is_empty(List *list);
int __mark_all_nodes(List* list);
bool insert(List *list, item *it);
item* del(List* list, const char* search_key, const size_t nkey, bool reclaim, bool *found);
item* replace(List* list, const char* search_key, const size_t nkey, item *new_it, bool reclaim, bool *inserted);
item* get(List *list, const char* search_key, const size_t nkey);
item* search(List* list, const char* search_key, const size_t nkey, item **left_item);
item* search_last(List* list, const char* search_key, const size_t nkey, item **left_item);


typedef struct {
    struct _stritem * next;
} fake_item;

//"Generic" key type (equivalent to void)
#define Type uintptr_t
/* Marking references */
//Mark reference as logically deleted
#define get_marked_reference(x)   	((Type) x | 1)
//Check if reference is logically deleted
#define is_marked_reference(x) 		((Type) x & 1)

//Mark reference as being replaced
#define get_marked_replacement_reference(x)   	((Type) x | 2)
//Check if reference is being replaced
#define is_marked_replacement_reference(x) 		((Type) x & 2)

//Get original reference
//  mandatory for every reference that might be marked for obvious reasons
#define get_unmarked_reference(x) 	((Type) x & ~(Type) 3) //Unmarks both "normal" and "replacement" mark

#endif
