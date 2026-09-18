// Logging for code that runs outside an initialised MPI system.
//
// RK_LOGx routes through librockit's logger, whose level table is only set up
// inside RK_MPI_SYS_Init. The supervisor never calls that (it owns no media
// hardware), so every RK_LOGx it issued was silently filtered: worker exits,
// restarts and heartbeat stalls left no trace at all. These macros write
// straight to stderr with a timestamp in the same spirit as the rockit lines,
// so the supervisor's story appears in the same log as its workers'.

#ifndef BABY_MONITOR_BASE_PLAIN_LOG_H
#define BABY_MONITOR_BASE_PLAIN_LOG_H

#include <cstdio>
#include <ctime>

namespace baby_monitor {

inline void PlainLogPrefix(const char* level) {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm local;
    localtime_r(&now.tv_sec, &local);
    fprintf(stderr, "%-16s %02d:%02d:%02d-%03ld {%-18s} ", "supervisor",
            local.tm_hour, local.tm_min, local.tm_sec, now.tv_nsec / 1000000,
            level);
}

}  // namespace baby_monitor

#define PLAIN_LOG(level, fmt, ...)                                    \
    do {                                                              \
        ::baby_monitor::PlainLogPrefix(level);                        \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__);                     \
    } while (0)

#define PLAIN_LOGE(fmt, ...) PLAIN_LOG("ERROR", fmt, ##__VA_ARGS__)
#define PLAIN_LOGW(fmt, ...) PLAIN_LOG("WARN", fmt, ##__VA_ARGS__)
#define PLAIN_LOGI(fmt, ...) PLAIN_LOG("INFO", fmt, ##__VA_ARGS__)

#endif  // BABY_MONITOR_BASE_PLAIN_LOG_H
