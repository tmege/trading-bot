#include "core/logging.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static FILE          *g_log_file   = NULL;
static _Atomic int    g_min_level  = TB_LOG_LVL_DEBUG;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_names[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static const char *level_colors[] = {
    "\033[36m",   /* DEBUG: cyan */
    "\033[32m",   /* INFO:  green */
    "\033[33m",   /* WARN:  yellow */
    "\033[31m",   /* ERROR: red */
    "\033[35m",   /* FATAL: magenta */
};

static const char *color_reset = "\033[0m";

int tb_log_init(const char *log_dir, int min_level) {
    g_min_level = min_level;

    /* Create log directory */
    if (mkdir(log_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: cannot create log dir %s: %s\n",
                log_dir, strerror(errno));
        return -1;
    }

    /* Open log file with date-based name */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&now, &tm_buf);
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/bot_%04d%02d%02d.log",
             log_dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    int fd = open(filepath, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open log file %s: %s\n",
                filepath, strerror(errno));
        return -1;
    }
    g_log_file = fdopen(fd, "a");
    if (!g_log_file) {
        close(fd);
        fprintf(stderr, "ERROR: cannot fdopen log file %s\n", filepath);
        return -1;
    }

    const char *lvl_name = (min_level >= 0 && min_level <= TB_LOG_LVL_FATAL)
                            ? level_names[min_level] : "???";
    tb_log_info("logging initialized: dir=%s level=%s", log_dir, lvl_name);
    return 0;
}

void tb_log_shutdown(void) {
    if (g_log_file) {
        tb_log_info("logging shutdown");
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void tb_log_write(tb_log_level_t level, const char *file, int line,
                  const char *fmt, ...) {
    if ((int)level < g_min_level) return;
    if ((int)level < 0 || (int)level > TB_LOG_LVL_FATAL) return;

    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&ts.tv_sec, &tm_buf);

    char timebuf[32];
    snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, ts.tv_nsec / 1000000);

    /* Format message */
    va_list args;
    va_start(args, fmt);
    char msgbuf[4096];
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);

    /* Write to file (no colors) */
    if (g_log_file) {
        fprintf(g_log_file, "%s [%s] %s:%d - %s\n",
                timebuf, level_names[level], file, line, msgbuf);
        fflush(g_log_file);
    }

    /* Write to stderr (with colors) */
    FILE *out = (level >= TB_LOG_LVL_WARN) ? stderr : stdout;
    fprintf(out, "%s %s[%s]%s %s:%d - %s\n",
            timebuf, level_colors[level], level_names[level],
            color_reset, file, line, msgbuf);

    pthread_mutex_unlock(&g_log_mutex);
}
