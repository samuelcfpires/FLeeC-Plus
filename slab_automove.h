#ifndef SLAB_AUTOMOVE_H
#define SLAB_AUTOMOVE_H

#ifdef USE_SLAB_ALLOCATOR

/* default automove functions */
void *slab_automove_init(struct settings *settings);
void slab_automove_free(void *arg);
void slab_automove_run(void *arg, int *src, int *dst);

typedef void *(*slab_automove_init_func)(struct settings *settings);
typedef void (*slab_automove_free_func)(void *arg);
typedef void (*slab_automove_run_func)(void *arg, int *src, int *dst);

typedef struct {
    slab_automove_init_func init;
    slab_automove_free_func free;
    slab_automove_run_func run;
} slab_automove_reg_t;

#else

#define slab_automove_init(s) NULL
#define slab_automove_free(a) do { } while(0)
#define slab_automove_run(a, s, d) do { } while(0)

#endif // USE_SLAB_ALLOCATOR

#endif
