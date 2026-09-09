#include "app/media_service.h"

#include "base/rk_platform.h"
#include "base/shared_memory.h"
#include "base/shared_records.h"

namespace baby_monitor {

int RunMediaService(const MediaPipelineConfig& config) {
    // Attach, never create: the supervisor owns these segments. A restarted
    // worker then rejoins the existing channels instead of replacing them and
    // orphaning the other reader.
    SharedMemory<EventChannel> event_channel;
    if (!event_channel.Attach(kEventChannelName)) {
        RK_LOGE("Media service could not attach to %s", kEventChannelName);
        return 1;
    }

    SharedMemory<HealthChannel> health_channel;
    if (!health_channel.Attach(kHealthChannelName)) {
        RK_LOGE("Media service could not attach to %s", kHealthChannelName);
        return 1;
    }

    MediaPipeline pipeline(config, event_channel.get(), health_channel.get());
    if (!pipeline.Initialise()) {
        // Non-zero tells the supervisor this was a failed start. The destructor
        // still releases whatever came up, so the next attempt finds the
        // hardware free rather than half-claimed.
        RK_LOGE("Media pipeline failed to initialise");
        return 1;
    }

    return pipeline.Run();
}

}  // namespace baby_monitor
