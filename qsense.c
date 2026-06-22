#include "recl.h"

#ifdef RECL_QSENSE

#define QSENSE_INITIAL_MODE QSENSE_EBR

qsense_state_t qsense_state;

int qsense_num_threads;
bool qsense_switching;
qsense_mode_e mode = QSENSE_INITIAL_MODE;

qsense_recl_t** qsense_recls;
__thread qsense_recl_t* t_recl_qsense;


static int start_qsense_management_thread(void);


void qsense_global_init(int num_threads) {
    qsense_num_threads = num_threads;
    ebr_global_init(qsense_num_threads);
    hp_global_init(qsense_num_threads);

    qsense_recls = calloc(qsense_num_threads, sizeof(qsense_recl_t*));
    qsense_state = (qsense_state_t){ .switching = false, .mode = QSENSE_INITIAL_MODE };

    start_qsense_management_thread();
}

void qsense_thread_init(int tid, size_t initial_bag_size) {
    ebr_thread_init(tid, initial_bag_size);
    hp_thread_init(tid, initial_bag_size);

    t_recl_qsense = l1_dcache_line_aligned_alloc(sizeof(qsense_recl_t));
    t_recl_qsense->became_quiescent = true;
    qsense_recls[tid] = t_recl_qsense;
}


#define QSENSE_QUIESCENCE_WAIT_SLEEP_USEC 10000

void qsense_wait_for_system_quiescence(void) {
    for (int i = 0; i < qsense_num_threads; i++) {
        // Skip threads that are already quiescent
        if (!ALOAD(qsense_recls[i]->became_quiescent)) {
            ASTORE(qsense_recls[i]->became_quiescent, false);
        }
    }

    for (int i = 0; i < qsense_num_threads; i++) {
        while (!ALOAD(qsense_recls[i]->became_quiescent)) {
            usleep(QSENSE_QUIESCENCE_WAIT_SLEEP_USEC);
        }
    }
}


/*--------- QSense Mode Management Thread ---------*/

extern size_t ebr_clear_bag(bag* b);

static void hp_clear_item_bag(bag* b) {
    size_t mem_freed = 0;

    for (int i = 0; i < b->size; i++) {
        slabs_free_batch_incremental(b->elems[i], &mem_freed);
    }

    if (mem_freed > 0)
        slabs_free_batch_finish(mem_freed);

    b->size = 0;
}

static void hp_clear_mem_bag(bag* b) {
    for (int i = 0; i < b->size; i++) {
        free(b->elems[i]);
    }

    b->size = 0;
}


static pthread_cond_t qsense_management_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t qsense_management_lock = PTHREAD_MUTEX_INITIALIZER;


static void* qsense_management_thread(void *arg) {
    while(true) {
        mutex_lock(&qsense_management_lock);
        pthread_cond_wait(&qsense_management_cond, &qsense_management_lock);

restart:;
        qsense_mode_e old_mode = get_qsense_mode();
        qsense_mode_e new_mode = other_mode(old_mode);
        if (settings.verbose > 0) {
            fprintf(stderr, "Starting QSense mode switch from %s to %s\n",
                old_mode == QSENSE_EBR ? "EBR" : "HP", new_mode == QSENSE_EBR ? "EBR" : "HP");
        }

        qsense_state_t new_state = {
            .switching = true,
            .mode = new_mode
        };
        ASTORE(qsense_state.data, new_state.data);

        // Wait for all threads to acknowledge the mode switch by becoming quiescent
        qsense_wait_for_system_quiescence();

        // Finish the switch
        new_state.switching = false;
        ASTORE(qsense_state.data, new_state.data);

        if (settings.verbose > 0) {
            fprintf(stderr, "QSense mode switch ended in mode %s\n", new_state.mode == QSENSE_EBR ? "EBR" : "HP");
        }

        // Free all retired items from old mode.
        // These can't be accessed because threads all threads have become quiescent at least once
        // since threads began retiring records into the new mode
        // meaning that they can't possibly be accessing records retired in the old mode.
        switch(new_state.mode) {
            case QSENSE_EBR:
                for (int i = 0; i < qsense_num_threads; i++) {
                    if (hp_recls[i] != NULL) {
                        hp_recl_t* recl = hp_recls[i];
                        if (recl->retired_items.size > 0)
                            hp_clear_item_bag(&recl->retired_items);
                        if (recl->retired_mem.size > 0)
                            hp_clear_mem_bag(&recl->retired_mem);
                    }
                }
                break;
            case QSENSE_HP:
                for (int i = 0; i < qsense_num_threads; i++) {
                    if (ebr_recls[i] != NULL) {
                        reclamation* recl = ebr_recls[i];
                        for (int j = 0; j < NUM_LIMBO_BAGS; j++) {
                            if (recl->limbo_bags[j].size > 0)
                                ebr_clear_bag(&recl->limbo_bags[j]);
                        }
                    }
                }
                break;
        }

        // The threshold may have been crossed again during the switch.
        // We don't know if this time it will be "for good", so just keep switching if necessary.
        switch(new_mode) {
            case QSENSE_EBR:
                if (get_mem_free() <= QSENSE_HP_SWITCH_MEM_THRESHOLD) {
                    goto restart;
                }
            case QSENSE_HP:
                if (get_mem_free() >= QSENSE_EBR_SWITCH_MEM_THRESHOLD) {
                    goto restart;
                }
                break;
        }

        mutex_unlock(&qsense_management_lock);
    }

    return NULL;
}


static int start_qsense_management_thread(void) {
    int ret;
    pthread_t thread;

    if((ret = pthread_create(&thread, NULL, qsense_management_thread, NULL)) != 0) {
        fprintf(stderr, "Failed to start QSense management thread: %s\n", strerror(ret));
        return -1;
    }

    return 0;
}

void qsense_switch_mode(qsense_mode_e new_mode) {
    if (pthread_mutex_trylock(&qsense_management_lock) == 0) {
        if (new_mode != get_qsense_mode()) {
            // if not, somehow we attempted to switch the mode to the one we're already in. ignore it
            pthread_cond_signal(&qsense_management_cond);
        }
        pthread_mutex_unlock(&qsense_management_lock);
    }
}


#else

static void _dummy_hp_c_function_to_avoid_empty_translation_unit_warning(void) __attribute__((unused));
static void _dummy_hp_c_function_to_avoid_empty_translation_unit_warning(void) {}

#endif