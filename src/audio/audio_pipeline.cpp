#include "audio/audio_pipeline.h"

#include <csignal>
#include <cstring>
#include <ctime>

#include "base/robust_mutex.h"

namespace baby_monitor {
namespace {

volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int /*signal_number*/) { g_stop_requested = 1; }

uint64_t MonotonicMicroseconds() {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000 +
           static_cast<uint64_t>(now.tv_nsec) / 1000;
}

// Frames the capture driver keeps queued. Four 20ms frames is 80ms of slack,
// enough to absorb scheduling jitter on a single core without audible latency.
constexpr uint32_t kCaptureFrameCount = 4;

// G.711A: 8 bits per sample, no compression state, trivially cheap to encode.
// Anything fancier would spend CPU on a stream nothing consumes yet.
constexpr RK_CODEC_ID_E kAudioCodec = RK_AUDIO_ID_PCM_ALAW;

}  // namespace

AudioPipeline::AudioPipeline(const AudioPipelineConfig& config,
                             EventChannel* event_channel, HealthChannel* health_channel)
    : config_(config), event_channel_(event_channel), health_channel_(health_channel) {}

AudioPipeline::~AudioPipeline() {
    TeardownBindings();
    TeardownModules();
}

bool AudioPipeline::Initialise() {
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("RK_MPI_SYS_Init failed");
        return false;
    }

    if (!InitialiseAudioInput()) return false;
    if (!InitialiseEncoder()) return false;

    // Cry detection is a feature, not a prerequisite. A missing model file must
    // not take audio capture down with it, so this failure is tolerated.
    if (!InitialiseCryDetector()) {
        RK_LOGW("Cry detection is unavailable; audio capture continues without it");
    }

    if (!BindPipeline()) return false;

    RK_LOGI("Audio pipeline ready: AI %u Hz x%u -> AENC, cry detection %s",
            config_.sample_rate, config_.channel_count,
            cry_detector_ready_ ? "on" : "off");
    return true;
}

bool AudioPipeline::InitialiseAudioInput() {
    AIO_ATTR_S audio_attributes;
    memset(&audio_attributes, 0, sizeof(audio_attributes));

    audio_attributes.soundCard.channels = config_.channel_count;
    audio_attributes.soundCard.sampleRate = config_.sample_rate;
    audio_attributes.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;

    audio_attributes.enSamplerate =
        static_cast<AUDIO_SAMPLE_RATE_E>(config_.sample_rate);
    audio_attributes.enBitwidth = AUDIO_BIT_WIDTH_16;
    audio_attributes.enSoundmode = config_.channel_count == 1 ? AUDIO_SOUND_MODE_MONO
                                                             : AUDIO_SOUND_MODE_STEREO;
    audio_attributes.u32FrmNum = kCaptureFrameCount;
    audio_attributes.u32PtNumPerFrm = config_.samples_per_frame;
    audio_attributes.u32ChnCnt = config_.channel_count;

    RK_S32 result = RK_MPI_AI_SetPubAttr(kAiDevice, &audio_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AI_SetPubAttr failed: %#x", result);
        return false;
    }

    result = RK_MPI_AI_Enable(kAiDevice);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AI_Enable failed: %#x", result);
        return false;
    }
    audio_input_ready_ = true;

    result = RK_MPI_AI_EnableChn(kAiDevice, kAiChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AI_EnableChn failed: %#x", result);
        return false;
    }

    RK_MPI_AI_SetVolume(kAiDevice, config_.capture_volume);
    RK_MPI_AI_SetTrackMode(kAiDevice, AUDIO_TRACK_NORMAL);
    return true;
}

bool AudioPipeline::InitialiseCryDetector() {
    AI_BCD_CONFIG_S detector_config;
    memset(&detector_config, 0, sizeof(detector_config));

    // Frames the engine accumulates before ruling. Longer is steadier but slower
    // to trigger; 60 is the value the SDK's own sample uses.
    detector_config.mFrameLen = 60;
    detector_config.mConfirmProb = config_.cry_confirm_probability;

    // Leaving stSedCfg zeroed lets the engine apply its 1 mic + 1 ref default,
    // which is what a single-microphone board wants.
    if (!config_.cry_model_path.empty()) {
        if (config_.cry_model_path.size() >= sizeof(detector_config.aModelPath)) {
            RK_LOGE("Cry model path is too long (%zu bytes)",
                    config_.cry_model_path.size());
            return false;
        }
        memcpy(detector_config.aModelPath, config_.cry_model_path.c_str(),
               config_.cry_model_path.size());
    }

    RK_S32 result = RK_MPI_AI_SetBcdAttr(kAiDevice, kAiChannel, &detector_config);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AI_SetBcdAttr failed: %#x (is %s present on the target?)",
                result, config_.cry_model_path.c_str());
        return false;
    }

    result = RK_MPI_AI_EnableBcd(kAiDevice, kAiChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AI_EnableBcd failed: %#x", result);
        return false;
    }

    cry_detector_ready_ = true;
    return true;
}

bool AudioPipeline::InitialiseEncoder() {
    AENC_CHN_ATTR_S encoder_attributes;
    memset(&encoder_attributes, 0, sizeof(encoder_attributes));

    encoder_attributes.enType = kAudioCodec;
    encoder_attributes.u32BufCount = kCaptureFrameCount;
    encoder_attributes.stCodecAttr.enType = kAudioCodec;
    encoder_attributes.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    encoder_attributes.stCodecAttr.u32Channels = config_.channel_count;
    encoder_attributes.stCodecAttr.u32SampleRate = config_.sample_rate;

    RK_S32 result = RK_MPI_AENC_CreateChn(kAencChannel, &encoder_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_AENC_CreateChn failed: %#x", result);
        return false;
    }

    encoder_ready_ = true;
    return true;
}

bool AudioPipeline::BindPipeline() {
    MPP_CHN_S capture_channel{RK_ID_AI, kAiDevice, kAiChannel};
    MPP_CHN_S encoder_channel{RK_ID_AENC, 0, kAencChannel};

    RK_S32 result = RK_MPI_SYS_Bind(&capture_channel, &encoder_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("Bind AI -> AENC failed: %#x", result);
        return false;
    }

    bound_ai_to_aenc_ = true;
    return true;
}

void AudioPipeline::DrainEncoder() {
    AUDIO_STREAM_S encoded_stream;
    memset(&encoded_stream, 0, sizeof(encoded_stream));

    // A short blocking wait paces this loop to the capture rate and yields the
    // core between frames, which on a single core is what lets the media
    // process run.
    RK_S32 result = RK_MPI_AENC_GetStream(kAencChannel, &encoded_stream, 100);
    if (result != RK_SUCCESS) {
        return;
    }

    // Nothing consumes encoded audio yet, but the frame must still be released:
    // an undrained encoder queue eventually blocks the capture path feeding it.
    RK_MPI_AENC_ReleaseStream(kAencChannel, &encoded_stream);
}

void AudioPipeline::PollCryDetector() {
    if (!cry_detector_ready_) {
        return;
    }

    AI_BCD_RESULT_S detection_result;
    memset(&detection_result, 0, sizeof(detection_result));

    RK_S32 result = RK_MPI_AI_GetBcdResult(kAiDevice, kAiChannel, &detection_result);
    if (result != RK_SUCCESS) {
        return;
    }

    const bool baby_crying = detection_result.bBabyCry == RK_TRUE;

    // Publish transitions only. The detector is polled far more often than its
    // verdict changes, and republishing an unchanged verdict would leave
    // consumers unable to tell a new cry from a continuing one.
    if (baby_crying == last_reported_cry_state_) {
        return;
    }
    last_reported_cry_state_ = baby_crying;

    CryEvent event;
    memset(&event, 0, sizeof(event));
    event.timestamp_us = MonotonicMicroseconds();
    event.baby_crying = baby_crying;

    // loud_sound_* stay zero: those come from the separate AED engine
    // (RK_MPI_AI_EnableAed), which is not enabled here.

    if (event_channel_ != nullptr) {
        SharedMutexGuard guard(&event_channel_->lock);
        if (guard.locked()) {
            event_channel_->cry = event;
            event_channel_->cry_sequence = ++published_cry_sequence_;
        }
    }

    RK_LOGI("Baby cry %s", baby_crying ? "detected" : "ended");
}

void AudioPipeline::PublishHeartbeat() {
    if (health_channel_ == nullptr) {
        return;
    }

    SharedMutexGuard guard(&health_channel_->lock);
    if (!guard.locked()) {
        return;
    }

    ProcessHeartbeat& beat =
        health_channel_->beats[static_cast<uint32_t>(MonitoredProcess::AUDIO)];
    beat.counter = ++heartbeat_counter_;
    beat.updated_at_us = MonotonicMicroseconds();
}

int AudioPipeline::Run() {
    signal(SIGTERM, HandleStopSignal);
    signal(SIGINT, HandleStopSignal);

    RK_LOGI("Audio pipeline running");

    uint64_t last_heartbeat_us = 0;
    constexpr uint64_t kHeartbeatIntervalUs = 1000000;

    while (g_stop_requested == 0) {
        DrainEncoder();
        PollCryDetector();

        const uint64_t now_us = MonotonicMicroseconds();
        if (now_us - last_heartbeat_us >= kHeartbeatIntervalUs) {
            PublishHeartbeat();
            last_heartbeat_us = now_us;
        }
    }

    RK_LOGI("Audio pipeline stopping");
    return 0;
}

void AudioPipeline::TeardownBindings() {
    if (bound_ai_to_aenc_) {
        MPP_CHN_S capture_channel{RK_ID_AI, kAiDevice, kAiChannel};
        MPP_CHN_S encoder_channel{RK_ID_AENC, 0, kAencChannel};
        RK_MPI_SYS_UnBind(&capture_channel, &encoder_channel);
        bound_ai_to_aenc_ = false;
    }
}

void AudioPipeline::TeardownModules() {
    if (cry_detector_ready_) {
        RK_MPI_AI_DisableBcd(kAiDevice, kAiChannel);
        cry_detector_ready_ = false;
    }

    if (encoder_ready_) {
        RK_MPI_AENC_DestroyChn(kAencChannel);
        encoder_ready_ = false;
    }

    if (audio_input_ready_) {
        RK_MPI_AI_DisableChn(kAiDevice, kAiChannel);
        RK_MPI_AI_Disable(kAiDevice);
        audio_input_ready_ = false;
    }

    RK_MPI_SYS_Exit();
}

}  // namespace baby_monitor
