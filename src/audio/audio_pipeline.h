// Audio pipeline: AI (capture) -> {AENC, built-in BCD cry detection}
//
// The cry detector is the SDK's own: RK_MPI_AI_EnableBcd plus
// RK_MPI_AI_GetBcdResult, backed by librkaudio_detect.so. That removes any need
// to ship raw PCM to another process for analysis, so the audio samples never
// leave this process either. Only the verdict is published.
//
// The AENC channel produces an encoded stream for future recording or a second
// RTSP track. It is drained every iteration regardless, because an unread
// encoder queue eventually blocks the capture path feeding it.

#ifndef BABY_MONITOR_AUDIO_AUDIO_PIPELINE_H
#define BABY_MONITOR_AUDIO_AUDIO_PIPELINE_H

#include <cstdint>
#include <string>

#include "base/rk_platform.h"
#include "base/shared_records.h"

namespace baby_monitor {

struct AudioPipelineConfig {
    uint32_t sample_rate = 16000;
    uint32_t channel_count = 1;

    // Channels the sound card is opened with, which is not the same number as
    // channel_count above: that one is the logical stream (mono is what a cry
    // detector wants), this one is what the codec hardware will accept. The
    // rv1106 acodec refuses a mono capture outright - "arecord -c 1" fails with
    // "Channels count non available" while "-c 2" records fine - so opening the
    // card mono makes RK_MPI_AI_EnableChn fail with 0xa00a8010.
    uint32_t card_channel_count = 2;

    // Samples per captured frame. The AIO layer only accepts specific values
    // (80/160/240/320/480/1024/2048); 320 is 20ms at 16kHz.
    uint32_t samples_per_frame = 320;

    int32_t capture_volume = 80;

    // ALSA card to capture from, in the SDK's "hw:<card>,<device>" form. Leaving
    // this empty is not a neutral default: AIO_ATTR_S::u8CardName then stays
    // zeroed, and rockit falls back to opening the card by device index, which
    // resolves to the name "pcm.record0". Its tinyalsa backend cannot match that
    // against real hardware and RK_MPI_AI_EnableChn fails with 0xa00a8010.
    std::string capture_card_name = "hw:0,0";

    // Path to the BCD model. Empty means the engine falls back to its built-in
    // energy heuristics without a neural model.
    std::string cry_model_path;

    // How confident the engine must be before a cry is reported, 0.0 to 1.0.
    float cry_confirm_probability = 0.7f;
};

class AudioPipeline {
public:
    AudioPipeline(const AudioPipelineConfig& config, EventChannel* event_channel,
                  HealthChannel* health_channel);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool Initialise();
    int Run();

private:
    bool InitialiseAudioInput();
    bool EnableAudioInputChannel();
    bool InitialiseCryDetector();
    bool InitialiseEncoder();
    bool BindPipeline();

    void TeardownBindings();
    void TeardownModules();

    void PollCryDetector();
    void DrainEncoder();
    void PublishHeartbeat();

    AudioPipelineConfig config_;
    EventChannel* event_channel_;
    HealthChannel* health_channel_;

    static constexpr int kAiDevice = 0;
    static constexpr int kAiChannel = 0;
    static constexpr int kAencChannel = 0;

    bool audio_input_ready_ = false;
    bool audio_channel_ready_ = false;
    bool cry_detector_ready_ = false;
    bool encoder_ready_ = false;
    bool bound_ai_to_aenc_ = false;

    bool last_reported_cry_state_ = false;
    uint64_t published_cry_sequence_ = 0;
    uint64_t heartbeat_counter_ = 0;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_AUDIO_AUDIO_PIPELINE_H
