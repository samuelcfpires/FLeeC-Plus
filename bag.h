#ifndef BAG_H
#define BAG_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "util.h"

#define GROWTH_FACTOR 2

typedef struct bag bag;
struct bag {
    size_t size; //number of elements currently in the bag
    size_t capacity; //maximum number of elements in the bag
    void** elems;
};

typedef struct bag_queue bag_queue;
struct bag_queue {
    struct bag b;
    size_t start;
};

static inline void bag_init(bag* b, size_t capacity) {
    b->elems = (void**) malloc(sizeof(void*) * capacity);
    if(b->elems == NULL) {
        fprintf(stderr, "Could not allocate bag of size %zu\n", capacity);
        exit(EXIT_FAILURE);
    } 

    b->capacity = capacity;
    b->size = 0;
}

//Grows bag to GROWTH_FACTOR times its current size
static inline void bag_grow(bag *b) {
    b->capacity = b->capacity * GROWTH_FACTOR;
    b->elems = realloc(b->elems, b->capacity * sizeof(void*));
    if(b->elems == NULL) {
        fprintf(stderr, "Could not grow bag to size %zu\n", b->capacity);
        exit(EXIT_FAILURE);
    } 
}

//Insert new elem e in end of bag, grow if needed
static inline void bag_put(bag *b, void* e) {
    if(b->size == b->capacity) {
        bag_grow(b);
    }

    b->elems[b->size] = e;
    b->size++;
}

// Remove element from bag at given index (swap with last element)
static inline void bag_remove(bag* b, size_t idx) {
    // Swap with last element and decrease size
    b->elems[idx] = b->elems[b->size - 1];
    b->size--;
}

// Check if pointer is in bag
static inline bool bag_contains(bag* b, void* ptr) {
    for (size_t i = 0; i < b->size; i++) {
        if (b->elems[i] == ptr) {
            return true;
        }
    }
    return false;
}

// Hash function for pointers (capacity must be power of 2)
// MurmurHash3's integer finalizer
static inline size_t bag_hash(void* ptr, size_t capacity) {
    // MurmurHash3's integer finalizer
    long long k = (long long)ptr;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccd;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53;
    k ^= k >> 33;
    return MOD_POW2(k, capacity);
}

/* Hash Bag 
 * Same as bag but empty slots are zeroed and uses linear probing.
*/

static inline void bag_init_hashset(bag* b, size_t capacity) {
    if (!IS_POW2(capacity)) {
        fprintf(stderr, "Hash set capacity must be a power of 2\n");
        exit(EXIT_FAILURE);
    }

    b->capacity = capacity;
    b->size = 0;
    b->elems = (void**)calloc(capacity, sizeof(void*));
    if (b->elems == NULL) {
        fprintf(stderr, "Could not allocate bag hashset of size %zu\n", capacity);
        exit(EXIT_FAILURE);
    }
}

static inline void bag_hashset_clear(bag* b) {
    memset(b->elems, 0, b->capacity * sizeof(void*));
    b->size = 0;
}

static inline void bag_hashset_grow(bag* b) {
    size_t old_capacity = b->capacity;
    void** old_elems = b->elems;

    b->capacity = old_capacity * 2;
    b->elems = (void**)calloc(b->capacity, sizeof(void*));
    if (b->elems == NULL) {
        fprintf(stderr, "Could not grow bag hashset to size %zu\n", b->capacity);
        exit(EXIT_FAILURE);
    }
    b->size = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        if (old_elems[i] != NULL) {
            size_t idx = bag_hash(old_elems[i], b->capacity);
            while (b->elems[idx] != NULL) {
                idx = MOD_POW2(idx + 1, b->capacity);
            }
            b->elems[idx] = old_elems[i];
            b->size++;
        }
    }

    free(old_elems);
}

static inline void bag_hashset_insert(bag* b, void* ptr) {
    if (ptr == NULL) return;

    if (b->size * 4 >= b->capacity * 3) {
        bag_hashset_grow(b);
    }
    
    size_t idx = bag_hash(ptr, b->capacity);
    
    while (b->elems[idx] != NULL) {
        if (b->elems[idx] == ptr) {
            return; // Already in set
        }
        idx = MOD_POW2(idx + 1, b->capacity);
    }
    
    b->elems[idx] = ptr;
    b->size++;
}

static inline bool bag_hashset_contains(bag* b, void* ptr) {
    if (ptr == NULL) return false;
    
    size_t idx = bag_hash(ptr, b->capacity);

    while (b->elems[idx] != NULL) {
        if (b->elems[idx] == ptr) {
            return true;
        }
        idx = MOD_POW2(idx + 1, b->capacity);
    }
    
    return false;
}

#endif
