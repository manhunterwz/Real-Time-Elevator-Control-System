/*
 * =============================================================================
 *  logger.c — Thread-Safe Event Logger
 *
 *  Provides timestamped, mutex-protected logging to a text file.
 *  Every significant system event (movement, door operations, mode changes)
 *  is recorded with millisecond-precision timestamps.
 * =============================================================================
 */

#include "../include/elevator.h"

/*
 * logger_init — Open the log file and prepare the logging mutex.
 *
 * Called once from main() before any threads are started.
 */
void logger_init(system_state_t *sys, const char *filename)
{
    pthread_mutex_init(&sys->log_mutex, NULL);

    sys->log_file = fopen(filename, "w");
    if (!sys->log_file) {
        perror("logger_init: failed to open log file");
        /* Fall back to stderr so logging still works */
        sys->log_file = stderr;
    }

    log_event(sys, "=== Elevator Control System — Log Started ===");
}

/*
 * logger_close — Flush and close the log file, destroy the mutex.
 *
 * Called from main() after all threads have been joined.
 */
void logger_close(system_state_t *sys)
{
    log_event(sys, "=== Elevator Control System — Log Ended ===");

    pthread_mutex_lock(&sys->log_mutex);
    if (sys->log_file && sys->log_file != stderr) {
        fflush(sys->log_file);
        fclose(sys->log_file);
    }
    sys->log_file = NULL;
    pthread_mutex_unlock(&sys->log_mutex);

    pthread_mutex_destroy(&sys->log_mutex);
}

/*
 * log_event — Write a timestamped message to the log file.
 *
 * Format:  [HH:MM:SS.mmm] <message>
 *
 * This function is thread-safe: multiple threads may call it concurrently.
 * Uses variadic arguments (printf-style) for flexible formatting.
 */
void log_event(system_state_t *sys, const char *fmt, ...)
{
    struct timespec ts;
    struct tm       tm_info;
    char            time_buf[32];
    char            msg_buf[MAX_LOG_MSG];
    va_list         args;

    /* Get current wall-clock time with millisecond resolution */
    clock_gettime(CLOCK_REALTIME, &ts);
#ifdef _WIN32
    /* Windows localtime_s has reversed parameter order */
    localtime_s(&tm_info, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm_info);
#endif

    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d.%03ld",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             (long)(ts.tv_nsec / 1000000));

    /* Format the caller's message */
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Write to log file under mutex protection */
    pthread_mutex_lock(&sys->log_mutex);
    if (sys->log_file) {
        fprintf(sys->log_file, "[%s] %s\n", time_buf, msg_buf);
        fflush(sys->log_file);
    }
    pthread_mutex_unlock(&sys->log_mutex);
}
