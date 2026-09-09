// The audio worker process: capture, encoding, cry detection, heartbeat.
//
// Kept separate from the media process deliberately. The cry detector runs a
// neural model through librkaudio_detect, which is the most likely component
// here to crash or wedge, and isolating it means such a failure cannot
// interrupt the video stream. The two processes exchange only small records.

#ifndef BABY_MONITOR_APP_AUDIO_SERVICE_H
#define BABY_MONITOR_APP_AUDIO_SERVICE_H

#include "audio/audio_pipeline.h"

namespace baby_monitor {

// Attaches to the shared channels the supervisor created, brings up the audio
// pipeline, and runs until SIGTERM. Returns a process exit status.
int RunAudioService(const AudioPipelineConfig& config);

}  // namespace baby_monitor

#endif  // BABY_MONITOR_APP_AUDIO_SERVICE_H
