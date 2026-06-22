/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread-specific logging facility implementation
 */
#include "thread_log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdatomic.h>

/* Thread-local storage for thread log handle */
static __thread thread_log_t *tlog_local = NULL;

/* Global configuration */
static thread_log_config_t g_config = {
    .log_dir = "./thread_logs"
};

/* Thread log structure */
struct thread_log_s {
    int thread_id;              /* Sequential thread ID */
    FILE *log_file;             /* Private log file handle */
    char log_path[256];         /* Path to log file */
};

/* Global state */
static struct {
    int next_thread_id;  /* Next sequential thread ID to allocate */
    bool initialized;    /* Whether subsystem is initialized */
} g_state = {
    .next_thread_id = 0,
    .initialized = false
};

/* Get current timestamp string (seconds.microseconds) */
static void get_timestamp(char *buf, size_t bufsize) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(buf, bufsize, "%ld.%06ld", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000));
}

/* Create log directory if it doesn't exist */
static int create_log_directory(const char *path) {
    struct stat st = {0};
    
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            return -1;
        }
    }
    return 0;
}

extern __thread int gt_tid;

/* Automatically initialize on first use */
static void auto_init(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&g_state.initialized, &expected, true)) {
        /* First thread to initialize */
        create_log_directory(g_config.log_dir);
    }
}

/* Automatically register thread on first log call */
static void auto_register(void) {
    if (tlog_local) {
        return; /* Already registered */
    }
    
    /* Ensure initialized */
    auto_init();
    
    /* Allocate new thread log structure */
    tlog_local = (thread_log_t *)calloc(1, sizeof(thread_log_t));
    if (!tlog_local) {
        return;
    }
    
    /* Allocate sequential thread ID atomically */
    int thread_id = gt_tid;
    if (thread_id >= MAX_THREAD_LOG_COUNT) {
        free(tlog_local);
        tlog_local = NULL;
        return;
    }
    
    /* Initialize thread log structure */
    tlog_local->thread_id = thread_id;
    
    /* Create log file path */
    snprintf(tlog_local->log_path, sizeof(tlog_local->log_path),
             "%s/thread_%03d.log", g_config.log_dir, thread_id);
    
    /* Open log file */
    tlog_local->log_file = fopen(tlog_local->log_path, "w");
    if (!tlog_local->log_file) {
        free(tlog_local);
        tlog_local = NULL;
        return;
    }
    
    /* Write initial log entry */
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(tlog_local->log_file, "[%s] Thread %d started (pthread_id=%lu)\n",
            timestamp, thread_id, (unsigned long)pthread_self());
    fflush(tlog_local->log_file);
}

int thread_log_get_id(void) {
    if (!tlog_local) {
        auto_register();
    }
    return tlog_local ? tlog_local->thread_id : -1;
}

void thread_log(const char *format, ...) {
    va_list args;
    
    if (!tlog_local) {
        auto_register();
    }
    
    if (!tlog_local || !tlog_local->log_file) {
        return; /* Failed to register */
    }
    
    /* Write timestamp */
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(tlog_local->log_file, "[%s] ", timestamp);
    
    /* Write formatted message */
    va_start(args, format);
    vfprintf(tlog_local->log_file, format, args);
    va_end(args);
    
    /* Ensure newline */
    fprintf(tlog_local->log_file, "\n");
    
    /* Always flush */
    fflush(tlog_local->log_file);
}

void thread_log_full(const char *func, int line, const char *format, ...) {
    va_list args;
    
    if (!tlog_local) {
        auto_register();
    }
    
    if (!tlog_local || !tlog_local->log_file) {
        return; /* Failed to register */
    }
    
    /* Write timestamp */
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(tlog_local->log_file, "[%s] ", timestamp);
    
    /* Write function and line info */
    fprintf(tlog_local->log_file, "[%s:%d] ", func, line);
    
    /* Write formatted message */
    va_start(args, format);
    vfprintf(tlog_local->log_file, format, args);
    va_end(args);
    
    /* Ensure newline */
    fprintf(tlog_local->log_file, "\n");
    
    /* Always flush */
    fflush(tlog_local->log_file);
}
