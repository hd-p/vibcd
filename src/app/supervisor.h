// The supervisor: owns the shared memory, forks the workers, feeds the dog.
//
// Two layers of watchdog, covering different failure modes:
//
//   Software - the supervisor reaps children, restarts them, and checks each
//              one's heartbeat counter. This catches both a crash (process
//              gone) and a hang (process alive, main loop no longer advancing).
//   Hardware - /dev/watchdog reboots the board if the supervisor itself dies or
//              wedges. Nothing in user space can rescue a broken supervisor, so
//              this is the only backstop for it.
//
// The ordering rule that makes this work: the hardware dog is fed only after
// the software checks complete. A supervisor that fed the dog unconditionally
// would keep a wedged system alive indefinitely, which is worse than a reboot.

#ifndef BABY_MONITOR_APP_SUPERVISOR_H
#define BABY_MONITOR_APP_SUPERVISOR_H

#include <cstdint>
#include <string>

#include "audio/audio_pipeline.h"
#include "media/media_pipeline.h"

namespace baby_monitor {

struct SupervisorConfig {
    MediaPipelineConfig media;
    AudioPipelineConfig audio;

    // Hardware watchdog device. Empty disables it, which is what you want under
    // gdb: a breakpoint would otherwise stop the feeding and reboot the board.
    std::string watchdog_device = "/dev/watchdog";

    // Requested hardware timeout. The driver may clamp this, so the effective
    // value is read back and logged rather than assumed.
    uint32_t watchdog_timeout_seconds = 30;

    uint32_t poll_interval_ms = 1000;

    // A worker whose heartbeat stops advancing for this long is treated as
    // wedged and restarted, even though its process still exists. Must comfortably
    // exceed the 1s heartbeat interval so ordinary scheduling jitter is not
    // mistaken for a hang.
    uint64_t heartbeat_timeout_us = 15ULL * 1000 * 1000;

    // Restart attempts per worker before giving up. A worker that cannot stay up
    // is usually failing on something a restart will not fix, such as a missing
    // model file or a sensor that never probed.
    int max_restarts = 5;
};

int RunSupervisor(const SupervisorConfig& config);

}  // namespace baby_monitor

#endif  // BABY_MONITOR_APP_SUPERVISOR_H
