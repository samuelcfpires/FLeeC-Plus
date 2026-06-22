#ifndef SLABS_MOVER_H
#define SLABS_MOVER_H

#define SLABS_REASSIGN_ALLOW_EVICTIONS 1

enum reassign_result_type {
    REASSIGN_OK=0, REASSIGN_RUNNING, REASSIGN_BADCLASS, REASSIGN_NOSPARE,
    REASSIGN_SRC_DST_SAME
};

#ifdef USE_SLAB_ALLOCATOR

struct slab_rebal_thread;
struct slab_rebal_thread *start_slab_maintenance_thread(void);
void stop_slab_maintenance_thread(struct slab_rebal_thread *t);

enum reassign_result_type slabs_reassign(struct slab_rebal_thread *t, int src, int dst, int flags);

void slab_maintenance_pause(struct slab_rebal_thread *t);
void slab_maintenance_resume(struct slab_rebal_thread *t);

bool is_slab_reassignment_running(void);

#else

#define start_slab_maintenance_thread() NULL
#define stop_slab_maintenance_thread(t) do { } while(0)
#define slabs_reassign(t, s, d, f) REASSIGN_RUNNING
#define slab_maintenance_pause(t) do { } while(0)
#define slab_maintenance_resume(t) do { } while(0)
#define is_slab_reassignment_running() false

#endif // USE_SLAB_ALLOCATOR

#endif // SLABS_MOVER_H
