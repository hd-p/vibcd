// The media worker process: video chain, motion detection, heartbeat.
//
// Runs as a forked child of the supervisor. Owns VI, VENC and IVS, so all three
// bound channel handles live in one address space and frames never leave the
// hardware. Publishes only conclusions (motion rectangles) to shared memory.

#ifndef BABY_MONITOR_APP_MEDIA_SERVICE_H
#define BABY_MONITOR_APP_MEDIA_SERVICE_H

#include "media/media_pipeline.h"

namespace baby_monitor {

// Attaches to the shared channels the supervisor created, brings up the
// pipeline, and runs until SIGTERM. Returns a process exit status.
//
// Called only in the child after fork, so it re-attaches rather than inheriting
// the parent's mapping: the parent's SharedMemory objects own their segments and
// unlink on destruction, which a child must not trigger.
int RunMediaService(const MediaPipelineConfig& config);

}  // namespace baby_monitor

#endif  // BABY_MONITOR_APP_MEDIA_SERVICE_H
