#include "app/audio_service.h"

#include "base/rk_platform.h"
#include "base/shared_memory.h"
#include "base/shared_records.h"

namespace baby_monitor {

int RunAudioService(const AudioPipelineConfig& config) {
    SharedMemory<EventChannel> event_channel;
    if (!event_channel.Attach(kEventChannelName)) {
        RK_LOGE("Audio service could not attach to %s", kEventChannelName);
        return 1;
    }

    SharedMemory<HealthChannel> health_channel;
    if (!health_channel.Attach(kHealthChannelName)) {
        RK_LOGE("Audio service could not attach to %s", kHealthChannelName);
        return 1;
    }

    AudioPipeline pipeline(config, event_channel.get(), health_channel.get());
    if (!pipeline.Initialise()) {
        RK_LOGE("Audio pipeline failed to initialise");
        return 1;
    }

    return pipeline.Run();
}

}  // namespace baby_monitor
