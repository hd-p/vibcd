// Shared-memory records exchanged between processes.
//
// Every payload here is deliberately small. The video pixel data never appears
// in shared memory: it travels VI -> {VENC, IVS} entirely inside the media
// process through RK_MPI_SYS_Bind, so the hardware moves it and the CPU
// never copies it. What crosses a process boundary is only the *conclusions*:
// motion rectangles, cry-detection verdicts, and periodic health counters.
//
// For scale: the previous design put a 2.97 MB VideoFrame in shared memory and
// copied it six times per frame, about 535 MB/s of memcpy at 30fps on a core
// that can sustain roughly 1-2 GB/s. Every struct below is under 1 KB.

#ifndef BABY_MONITOR_BASE_SHARED_RECORDS_H
#define BABY_MONITOR_BASE_SHARED_RECORDS_H

#include <cstdint>

#include "base/robust_mutex.h"

namespace baby_monitor {

inline constexpr const char* kEventChannelName = "/baby_monitor_events";
inline constexpr const char* kHealthChannelName = "/baby_monitor_health";

// Upper bound on motion rectangles republished per frame. The IVS hardware can
// report up to 4096, but a supervisor only needs the largest few to drive
// alerting, and this keeps the record inside one page.
inline constexpr uint32_t kMaxMotionRectangles = 16;

struct MotionRectangle {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

// One motion-detection verdict, produced by the media process from IVS output.
struct MotionEvent {
    uint64_t timestamp_us;
    uint32_t frame_id;
    uint32_t rectangle_count;
    MotionRectangle rectangles[kMaxMotionRectangles];

    // Moving area in pixels, straight from IVS_MD_INFO_S::u32Square. Compared
    // against a fraction of the frame area to decide whether motion is real.
    uint32_t moving_pixel_area;
    uint32_t frame_pixel_area;
    bool motion_present;
};

// One cry-detection verdict from the AI module's built-in BCD engine.
struct CryEvent {
    uint64_t timestamp_us;
    bool baby_crying;
    bool loud_sound_detected;
    float loud_sound_score;
};

// Single-slot mailboxes. A consumer wants the newest verdict, never a backlog,
// so a ring buffer would only add the risk of serving stale events. The
// sequence counter lets a reader tell a fresh verdict from one already seen.
struct EventChannel {
    SharedMutexStorage lock;

    uint64_t motion_sequence;
    MotionEvent motion;

    uint64_t cry_sequence;
    CryEvent cry;
};

// Which long-running processes the supervisor tracks.
enum class MonitoredProcess : uint32_t {
    MEDIA = 0,
    AUDIO = 1,
    COUNT = 2
};

// Liveness beat. A process can be running yet wedged, so the supervisor checks
// this counter as well as process existence: a stalled main loop stops
// incrementing even though the pid is still alive.
struct ProcessHeartbeat {
    uint64_t counter;
    uint64_t updated_at_us;
};

struct HealthChannel {
    SharedMutexStorage lock;
    ProcessHeartbeat beats[static_cast<uint32_t>(MonitoredProcess::COUNT)];
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_BASE_SHARED_RECORDS_H
