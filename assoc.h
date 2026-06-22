#ifndef ASSOC_H
#define ASSOC_H


#define CLOCK_FREQUENCY_POWER_DEFAULT   4  // 16
#define ARENA_SIZE_POWER_DEFAULT        12 // 4KB


/* associative array */
void assoc_init(const int hashpower_init);

item* assoc_find(const char *key, const size_t nkey, const uint64_t hv);
item* assoc_insert(item *item, const uint64_t hv);
int assoc_delete(const char *key, const size_t nkey, const uint64_t hv);
// Returns the nbytes of the old item if replaced, 0 if inserted new (NOT AN ERROR CODE!)
int assoc_replace(item *new_it, const uint64_t hv);
// Returns the nbytes of the old item if replaced, 0 if inserted new (NOT AN ERROR CODE!)
int assoc_set(item *new_it, const uint64_t hv);
void assoc_bump(item *it, const uint64_t hv);
int try_evict(const uint64_t total_bytes, size_t *mem_freed);

uint64_t get_curr_items(void);

int start_assoc_maintenance_thread(void);
void assoc_check_expand(void);
void *assoc_maintenance_thread(void *arg);
void start_expansion(void);

int start_clock_ageing_thread(void);

#endif /* ASSOC_H */
