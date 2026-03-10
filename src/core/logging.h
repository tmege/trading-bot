#ifndef TB_LOGGING_H
#define TB_LOGGING_H

#include <stdio.h>
#include <time.h>
#include <string.h>

/* Log levels */
typedef enum {
    TB_LOG_LVL_DEBUG = 0,
    TB_LOG_LVL_INFO  = 1,
    TB_LOG_LVL_WARN  = 2,
    TB_LOG_LVL_ERROR = 3,
    TB_LOG_LVL_FATAL = 4,
} tb_log_level_t;

/* Initialize logging (creates log directory, opens file) */
int  tb_log_init(const char *log_dir, int min_level);
void tb_log_shutdown(void);

/* Core logging function */
void tb_log_write(tb_log_level_t level, const char *file, int line,
                  const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Convenience macros */
#define TB_LOG_FILE (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define tb_log_debug(fmt, ...) \
    tb_log_write(TB_LOG_LVL_DEBUG, TB_LOG_FILE, __LINE__, fmt, ##__VA_ARGS__)
#define tb_log_info(fmt, ...) \
    tb_log_write(TB_LOG_LVL_INFO, TB_LOG_FILE, __LINE__, fmt, ##__VA_ARGS__)
#define tb_log_warn(fmt, ...) \
    tb_log_write(TB_LOG_LVL_WARN, TB_LOG_FILE, __LINE__, fmt, ##__VA_ARGS__)
#define tb_log_error(fmt, ...) \
    tb_log_write(TB_LOG_LVL_ERROR, TB_LOG_FILE, __LINE__, fmt, ##__VA_ARGS__)
#define tb_log_fatal(fmt, ...) \
    tb_log_write(TB_LOG_LVL_FATAL, TB_LOG_FILE, __LINE__, fmt, ##__VA_ARGS__)

#endif /* TB_LOGGING_H */
