#ifndef ITEMS_H
#define ITEMS_H

/* See items.c */
uint64_t get_cas_id(void);
void set_cas_id(uint64_t new_cas);

/*@null@*/
item *do_item_alloc(const char *key, const size_t nkey, const unsigned int flags, const rel_time_t exptime, const int nbytes);
item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain);
item *do_item_alloc_pull(const size_t ntotal, const unsigned int id);
#ifdef USE_SLAB_ALLOCATOR
// slab allocator doesn't its own ABA protection, it relies the hash table's (EBR)
// so item's can't be freed directly
#define item_free(it) recl_retire_item(it)
#else
#define item_free(it) slabs_free(it)
#endif
bool item_size_ok(const size_t nkey, const int flags, const int nbytes);

item *do_item_link(item *it, const uint64_t hv);     /** may fail if transgresses limits */
void do_item_unlink(item *it, const uint64_t hv);
void do_item_unlink_nolock(item *it, const uint64_t hv);
void do_item_update(item *it, const uint64_t hv); /** update LRU time to current and reposition */
void do_item_update_nolock(item *it, const uint64_t hv);
bool do_item_replace(item *new_it, const uint64_t hv);
bool do_item_set(item *new_it, const uint64_t hv);

int item_is_flushed(item *it);

item *do_item_get(const char *key, const size_t nkey, const uint64_t hv, LIBEVENT_THREAD *t, const bool do_update);
item *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint64_t hv, LIBEVENT_THREAD *t);
void do_item_bump(LIBEVENT_THREAD *t, item *it, const uint64_t hv);


//Unused, but leave this here
typedef struct {
    int64_t evicted;
    int64_t outofmemory;
    uint32_t age;
} item_stats_automove;

#endif /* ITEMS_H */
