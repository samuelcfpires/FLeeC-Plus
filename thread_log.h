/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread-specific logging facility
 * Each thread logs to its own private file identified by sequential thread ID
 */
#ifndef THREAD_LOG_H
#define THREAD_LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of threads that can be logged */
#define MAX_THREAD_LOG_COUNT 256

/* Thread log structure - opaque to users */
typedef struct thread_log_s thread_log_t;

/* Thread log configuration */
typedef struct {
    const char *log_dir;        /* Directory where log files are stored */
} thread_log_config_t;

/*
 * Get the sequential thread ID for the current thread
 * Automatically initializes and registers on first call
 * 
 * @return Thread ID (>= 0)
 */
int thread_log_get_id(void);

/*
 * Write a formatted log message to the current thread's log file
 * 
 * @param format Printf-style format string
 * @param ... Variable arguments for format string
 */
void thread_log(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

/*
 * Write a formatted log message with explicit function/line info
 * 
 * @param func Function name
 * @param line Line number
 * @param format Printf-style format string
 * @param ... Variable arguments for format string
 */
void thread_log_full(const char *func, int line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/* Convenience macro for logging with automatic function/line info */
//#define THREAD_LOG(...)
#define THREAD_LOG(...) thread_log_full(__func__, __LINE__, __VA_ARGS__)
#define THREAD_LOGfr(...) thread_log_full(__func__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* THREAD_LOG_H */
