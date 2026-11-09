// The log (stderr). On Windows a line is formatted by the thread that logs and queued, and a thread of its own writes
// the queue out (runtime/logwriter.c), so logging never waits for the disk or a console. Elsewhere lines go straight
// to stderr, as before.
#pragma once
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int rt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // (rt.h LOG on Windows)
int rt_vlogf(const char *fmt, va_list ap);
void rt_log_write(const char *s, size_t n);  // text already formatted (hitchlog.c's lines)
// (main.c, once stderr is where it stays: after plat_standalone) from now on lines are queued
void rt_log_start(void);
// Every queued line written out now, and each line after it written at once by the thread that logs: for the paths
// that end the process (FATAL, a crash, exit), which must not lose the log's last lines. Elsewhere: fflush(stderr).
void rt_log_flush(void);

#ifdef __cplusplus
}
#endif
