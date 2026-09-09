#include "media/media_pipeline.h"

#include <csignal>
#include <cstring>
#include <ctime>

#include <unistd.h>

#include "base/robust_mutex.h"

namespace baby_monitor {
namespace {

// Set by the signal handler, polled by the run loop. Only sig_atomic_t is safe
// to touch from a handler.
volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int /*signal_number*/) { g_stop_requested = 1; }

uint64_t MonotonicMicroseconds() {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000 +
           static_cast<uint64_t>(now.tv_nsec) / 1000;
}

// VENC and IVS both want dimensions aligned to a macroblock grid.
uint32_t AlignUpTo16(uint32_t value) { return (value + 15) & ~15u; }

}  // namespace

MediaPipeline::MediaPipeline(const MediaPipelineConfig& config,
                             EventChannel* event_channel, HealthChannel* health_channel)
    : config_(config), event_channel_(event_channel), health_channel_(health_channel) {}

MediaPipeline::~MediaPipeline() {
    TeardownBindings();
    TeardownModules();
}

bool MediaPipeline::Initialise() {
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("RK_MPI_SYS_Init failed");
        return false;
    }

    // Order matters: every module must exist before anything is bound, because
    // binding starts data flowing immediately.
    if (!InitialiseVideoInput()) return false;
    if (!InitialiseScaler()) return false;
    if (!InitialiseEncoder()) return false;
    if (!InitialiseMotionDetector()) return false;
    if (!InitialiseRtspServer()) return false;
    if (!BindPipeline()) return false;

    RK_LOGI("Media pipeline ready: VI %ux%u -> VPSS -> {VENC %ux%u, IVS %ux%u}",
            config_.sensor_width, config_.sensor_height, config_.stream_width,
            config_.stream_height, config_.detect_width, config_.detect_height);
    return true;
}

bool MediaPipeline::InitialiseVideoInput() {
    VI_DEV_ATTR_S device_attributes;
    memset(&device_attributes, 0, sizeof(device_attributes));

    RK_S32 result = RK_MPI_VI_GetDevAttr(kViDevice, &device_attributes);
    if (result == RK_ERR_VI_NOT_CONFIG) {
        result = RK_MPI_VI_SetDevAttr(kViDevice, &device_attributes);
        if (result != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevAttr failed: %#x", result);
            return false;
        }
    }

    if (RK_MPI_VI_GetDevIsEnable(kViDevice) != RK_SUCCESS) {
        result = RK_MPI_VI_EnableDev(kViDevice);
        if (result != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_EnableDev failed: %#x", result);
            return false;
        }

        VI_DEV_BIND_PIPE_S pipe_binding;
        memset(&pipe_binding, 0, sizeof(pipe_binding));
        pipe_binding.u32Num = 1;
        pipe_binding.PipeId[0] = kViPipe;
        result = RK_MPI_VI_SetDevBindPipe(kViDevice, &pipe_binding);
        if (result != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevBindPipe failed: %#x", result);
            return false;
        }
    }

    VI_CHN_ATTR_S channel_attributes;
    memset(&channel_attributes, 0, sizeof(channel_attributes));
    channel_attributes.stIspOpt.u32BufCount = 2;
    channel_attributes.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    channel_attributes.stSize.u32Width = config_.sensor_width;
    channel_attributes.stSize.u32Height = config_.sensor_height;
    channel_attributes.enPixelFormat = RK_FMT_YUV420SP;
    channel_attributes.enCompressMode = COMPRESS_MODE_NONE;

    // Depth 0 means frames go only to the bound consumer and are never queued
    // for userspace retrieval. That is exactly what we want: nothing in this
    // process ever calls RK_MPI_VI_GetChnFrame, so a non-zero depth would build
    // a queue nobody drains and stall the pipeline once it filled.
    channel_attributes.u32Depth = 0;

    RK_S32 set_result = RK_MPI_VI_SetChnAttr(kViDevice, kViChannel, &channel_attributes);
    if (set_result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr failed: %#x", set_result);
        return false;
    }

    RK_S32 enable_result = RK_MPI_VI_EnableChn(kViDevice, kViChannel);
    if (enable_result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_EnableChn failed: %#x", enable_result);
        return false;
    }

    video_input_ready_ = true;
    return true;
}

bool MediaPipeline::InitialiseScaler() {
    VPSS_GRP_ATTR_S group_attributes;
    memset(&group_attributes, 0, sizeof(group_attributes));
    group_attributes.u32MaxW = config_.sensor_width;
    group_attributes.u32MaxH = config_.sensor_height;
    group_attributes.enPixelFormat = RK_FMT_YUV420SP;
    group_attributes.enCompressMode = COMPRESS_MODE_NONE;
    group_attributes.stFrameRate.s32SrcFrameRate = -1;
    group_attributes.stFrameRate.s32DstFrameRate = -1;

    RK_S32 result = RK_MPI_VPSS_CreateGrp(kVpssGroup, &group_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_CreateGrp failed: %#x", result);
        return false;
    }
    scaler_ready_ = true;

    // Channel 0: full-rate output for the encoder.
    VPSS_CHN_ATTR_S encode_channel;
    memset(&encode_channel, 0, sizeof(encode_channel));
    encode_channel.enChnMode = VPSS_CHN_MODE_USER;
    encode_channel.u32Width = config_.stream_width;
    encode_channel.u32Height = config_.stream_height;
    encode_channel.enPixelFormat = RK_FMT_YUV420SP;
    encode_channel.enDynamicRange = DYNAMIC_RANGE_SDR8;
    encode_channel.enCompressMode = COMPRESS_MODE_NONE;
    encode_channel.stFrameRate.s32SrcFrameRate = -1;
    encode_channel.stFrameRate.s32DstFrameRate = -1;

    // Depth 0 for the same reason as VI: the consumer is a bound module, so no
    // userspace queue should accumulate.
    encode_channel.u32Depth = 0;

    result = RK_MPI_VPSS_SetChnAttr(kVpssGroup, kVpssEncodeChannel, &encode_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_SetChnAttr(encode) failed: %#x", result);
        return false;
    }
    result = RK_MPI_VPSS_EnableChn(kVpssGroup, kVpssEncodeChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_EnableChn(encode) failed: %#x", result);
        return false;
    }

    // Channel 1: downscaled and frame-rate limited for motion detection.
    // Hardware fan-out is the point here: one VI stream feeds two consumers
    // with no CPU involvement, which is the real single-producer/multi-consumer
    // mechanism on this chip.
    VPSS_CHN_ATTR_S detect_channel;
    memset(&detect_channel, 0, sizeof(detect_channel));
    detect_channel.enChnMode = VPSS_CHN_MODE_USER;
    detect_channel.u32Width = AlignUpTo16(config_.detect_width);
    detect_channel.u32Height = AlignUpTo16(config_.detect_height);
    detect_channel.enPixelFormat = RK_FMT_YUV420SP;
    detect_channel.enDynamicRange = DYNAMIC_RANGE_SDR8;
    detect_channel.enCompressMode = COMPRESS_MODE_NONE;

    // Motion detection gains nothing from 30fps. Dropping to 10 in hardware
    // cuts IVS bandwidth to a third for free.
    detect_channel.stFrameRate.s32SrcFrameRate = 30;
    detect_channel.stFrameRate.s32DstFrameRate = 10;
    detect_channel.u32Depth = 0;

    result = RK_MPI_VPSS_SetChnAttr(kVpssGroup, kVpssDetectChannel, &detect_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_SetChnAttr(detect) failed: %#x", result);
        return false;
    }
    result = RK_MPI_VPSS_EnableChn(kVpssGroup, kVpssDetectChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_EnableChn(detect) failed: %#x", result);
        return false;
    }

    result = RK_MPI_VPSS_StartGrp(kVpssGroup);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_StartGrp failed: %#x", result);
        return false;
    }

    return true;
}

bool MediaPipeline::InitialiseEncoder() {
    VENC_CHN_ATTR_S encoder_attributes;
    memset(&encoder_attributes, 0, sizeof(encoder_attributes));

    const RK_CODEC_ID_E codec =
        config_.use_h265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;

    encoder_attributes.stVencAttr.enType = codec;
    encoder_attributes.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    encoder_attributes.stVencAttr.u32PicWidth = config_.stream_width;
    encoder_attributes.stVencAttr.u32PicHeight = config_.stream_height;
    encoder_attributes.stVencAttr.u32VirWidth = AlignUpTo16(config_.stream_width);
    encoder_attributes.stVencAttr.u32VirHeight = AlignUpTo16(config_.stream_height);
    encoder_attributes.stVencAttr.u32StreamBufCnt = 2;

    // Compressed output only ever needs a fraction of a raw frame. Sizing this
    // at width*height/2 rather than the raw 3/2 saves about 1.5 MB of the 66 MB
    // CMA pool at 1080p, and the encoder reports overflow rather than
    // corrupting if a pathological frame exceeds it.
    encoder_attributes.stVencAttr.u32BufSize =
        config_.stream_width * config_.stream_height / 2;

    if (codec == RK_VIDEO_ID_AVC) {
        encoder_attributes.stVencAttr.u32Profile = 100;  // High profile
        encoder_attributes.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        encoder_attributes.stRcAttr.stH264Cbr.u32BitRate = config_.bitrate_kbps;
        encoder_attributes.stRcAttr.stH264Cbr.u32Gop = 60;
    } else {
        encoder_attributes.stVencAttr.u32Profile = 0;  // Main profile
        encoder_attributes.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        encoder_attributes.stRcAttr.stH265Cbr.u32BitRate = config_.bitrate_kbps;
        encoder_attributes.stRcAttr.stH265Cbr.u32Gop = 60;
    }

    RK_S32 result = RK_MPI_VENC_CreateChn(kVencChannel, &encoder_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VENC_CreateChn failed: %#x", result);
        return false;
    }
    encoder_ready_ = true;

    VENC_RECV_PIC_PARAM_S receive_parameters;
    memset(&receive_parameters, 0, sizeof(receive_parameters));
    receive_parameters.s32RecvPicNum = -1;  // unbounded

    result = RK_MPI_VENC_StartRecvFrame(kVencChannel, &receive_parameters);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VENC_StartRecvFrame failed: %#x", result);
        return false;
    }

    return true;
}

bool MediaPipeline::InitialiseMotionDetector() {
    IVS_CHN_ATTR_S detector_attributes;
    memset(&detector_attributes, 0, sizeof(detector_attributes));

    detector_attributes.enMode = IVS_MODE_MD_OD;
    detector_attributes.u32PicWidth = AlignUpTo16(config_.detect_width);
    detector_attributes.u32PicHeight = AlignUpTo16(config_.detect_height);
    detector_attributes.enPixelFormat = RK_FMT_YUV420SP;
    detector_attributes.s32Gop = 30;
    detector_attributes.bSmearEnable = RK_FALSE;
    detector_attributes.bWeightpEnable = RK_FALSE;
    detector_attributes.bMDEnable = RK_TRUE;

    // Analyse every other frame of the already-decimated 10fps channel, giving
    // roughly 5 detections per second. Plenty for a sleeping infant, and it
    // halves IVS load again.
    detector_attributes.s32MDInterval = 2;
    detector_attributes.bMDNightMode = RK_FALSE;
    detector_attributes.u32MDSensibility = config_.motion_sensitivity;

    // Occlusion detection comes along with IVS_MODE_MD_OD at negligible cost
    // and catches a covered lens, which matters for a monitoring device.
    detector_attributes.bODEnable = RK_TRUE;
    detector_attributes.s32ODInterval = 1;
    detector_attributes.s32ODPercent = 7;

    detector_attributes.u32MaxWidth = AlignUpTo16(config_.detect_width);
    detector_attributes.u32MaxHeight = AlignUpTo16(config_.detect_height);

    RK_S32 result = RK_MPI_IVS_CreateChn(kIvsChannel, &detector_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_IVS_CreateChn failed: %#x", result);
        return false;
    }

    motion_detector_ready_ = true;
    return true;
}

bool MediaPipeline::InitialiseRtspServer() {
    rtsp_server_ = create_rtsp_demo(config_.rtsp_port);
    if (rtsp_server_ == nullptr) {
        RK_LOGE("create_rtsp_demo(port %d) failed", config_.rtsp_port);
        return false;
    }

    rtsp_session_ = rtsp_new_session(rtsp_server_, config_.rtsp_path.c_str());
    if (rtsp_session_ == nullptr) {
        RK_LOGE("rtsp_new_session(%s) failed", config_.rtsp_path.c_str());
        return false;
    }

    // rtsp_demo has its own codec enum, distinct from RK_VIDEO_ID_*. Passing an
    // RK codec id here yields a session that negotiates the wrong media type.
    const int rtsp_codec =
        config_.use_h265 ? RTSP_CODEC_ID_VIDEO_H265 : RTSP_CODEC_ID_VIDEO_H264;
    if (rtsp_set_video(rtsp_session_, rtsp_codec, nullptr, 0) != 0) {
        RK_LOGE("rtsp_set_video failed");
        return false;
    }

    rtsp_sync_video_ts(rtsp_session_, rtsp_get_reltime(), rtsp_get_ntptime());

    // The RTSP server accepts any client that can reach the port: no
    // authentication, no transport encryption. Audio and video from a nursery
    // are about as sensitive as home telemetry gets, so this belongs on a
    // trusted VLAN, never port-forwarded to the internet.
    RK_LOGW("RTSP server on port %d has no authentication; restrict network access",
            config_.rtsp_port);
    RK_LOGI("Stream available at rtsp://<device-ip>:%d%s", config_.rtsp_port,
            config_.rtsp_path.c_str());
    return true;
}

bool MediaPipeline::BindPipeline() {
    MPP_CHN_S video_input_channel;
    video_input_channel.enModId = RK_ID_VI;
    video_input_channel.s32DevId = kViDevice;
    video_input_channel.s32ChnId = kViChannel;

    MPP_CHN_S scaler_input_channel;
    scaler_input_channel.enModId = RK_ID_VPSS;
    scaler_input_channel.s32DevId = kVpssGroup;
    scaler_input_channel.s32ChnId = kVpssEncodeChannel;

    RK_S32 result = RK_MPI_SYS_Bind(&video_input_channel, &scaler_input_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("Bind VI -> VPSS failed: %#x", result);
        return false;
    }
    bound_vi_to_vpss_ = true;

    MPP_CHN_S scaler_encode_output;
    scaler_encode_output.enModId = RK_ID_VPSS;
    scaler_encode_output.s32DevId = kVpssGroup;
    scaler_encode_output.s32ChnId = kVpssEncodeChannel;

    MPP_CHN_S encoder_channel;
    encoder_channel.enModId = RK_ID_VENC;
    encoder_channel.s32DevId = 0;
    encoder_channel.s32ChnId = kVencChannel;

    result = RK_MPI_SYS_Bind(&scaler_encode_output, &encoder_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("Bind VPSS -> VENC failed: %#x", result);
        return false;
    }
    bound_vpss_to_venc_ = true;

    MPP_CHN_S scaler_detect_output;
    scaler_detect_output.enModId = RK_ID_VPSS;
    scaler_detect_output.s32DevId = kVpssGroup;
    scaler_detect_output.s32ChnId = kVpssDetectChannel;

    MPP_CHN_S detector_channel;
    detector_channel.enModId = RK_ID_IVS;
    detector_channel.s32DevId = 0;
    detector_channel.s32ChnId = kIvsChannel;

    result = RK_MPI_SYS_Bind(&scaler_detect_output, &detector_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("Bind VPSS -> IVS failed: %#x", result);
        return false;
    }
    bound_vpss_to_ivs_ = true;

    return true;
}

bool MediaPipeline::ForwardEncodedFrame() {
    VENC_STREAM_S encoded_stream;
    memset(&encoded_stream, 0, sizeof(encoded_stream));

    VENC_PACK_S stream_packet;
    memset(&stream_packet, 0, sizeof(stream_packet));
    encoded_stream.pstPack = &stream_packet;

    // Short timeout instead of a blocking wait: the same loop also has to drain
    // IVS results and refresh the heartbeat, so it must not park in one call.
    RK_S32 result = RK_MPI_VENC_GetStream(kVencChannel, &encoded_stream, 20);
    if (result != RK_SUCCESS) {
        return result == RK_ERR_VENC_BUF_EMPTY;
    }

    void* payload = RK_MPI_MB_Handle2VirAddr(stream_packet.pMbBlk);
    if (payload != nullptr && rtsp_session_ != nullptr) {
        // u32Offset skips any header padding the encoder inserted; sending from
        // the buffer start would prepend garbage to the frame.
        const uint8_t* frame_bytes =
            static_cast<const uint8_t*>(payload) + stream_packet.u32Offset;
        const int frame_length =
            static_cast<int>(stream_packet.u32Len - stream_packet.u32Offset);

        if (frame_length > 0) {
            rtsp_tx_video(rtsp_session_, frame_bytes, frame_length,
                          stream_packet.u64PTS);
            rtsp_do_event(rtsp_server_);
        }
    }

    RK_S32 release_result = RK_MPI_VENC_ReleaseStream(kVencChannel, &encoded_stream);
    if (release_result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VENC_ReleaseStream failed: %#x", release_result);
        return false;
    }

    return true;
}

void MediaPipeline::PublishMotionResults() {
    IVS_RESULT_INFO_S results;
    memset(&results, 0, sizeof(results));

    // Zero timeout: poll and move on. The encoder path must not stall waiting
    // for detection results.
    RK_S32 result = RK_MPI_IVS_GetResults(kIvsChannel, &results, 0);
    if (result != RK_SUCCESS) {
        return;
    }

    if (results.s32ResultNum > 0 && results.pstResults != nullptr) {
        const IVS_MD_INFO_S& motion_info = results.pstResults->stMdInfo;

        const uint32_t detect_area =
            AlignUpTo16(config_.detect_width) * AlignUpTo16(config_.detect_height);

        MotionEvent event;
        memset(&event, 0, sizeof(event));
        event.timestamp_us = MonotonicMicroseconds();
        event.frame_id = motion_info.frameId;
        event.moving_pixel_area = motion_info.u32Square;
        event.frame_pixel_area = detect_area;

        // Integer per-mille comparison, matching how the SDK's own IVS sample
        // decides motion (u32Square against a percentage of frame area).
        event.motion_present =
            detect_area > 0 &&
            (static_cast<uint64_t>(motion_info.u32Square) * 1000u) / detect_area >
                config_.motion_area_threshold_permille;

        // IVS can report up to 4096 rectangles; only the first few are
        // republished, which is all a supervisor needs to localise the motion.
        const uint32_t rectangles_to_copy =
            motion_info.u32RectNum < kMaxMotionRectangles ? motion_info.u32RectNum
                                                          : kMaxMotionRectangles;
        event.rectangle_count = rectangles_to_copy;

        // Rescale from detection resolution to stream resolution so consumers
        // can overlay boxes on the encoded video without knowing about the
        // internal downscale.
        const uint32_t detect_width = AlignUpTo16(config_.detect_width);
        const uint32_t detect_height = AlignUpTo16(config_.detect_height);
        for (uint32_t index = 0; index < rectangles_to_copy; ++index) {
            const RECT_S& source = motion_info.stRect[index];
            MotionRectangle& target = event.rectangles[index];
            target.x = static_cast<int32_t>(
                static_cast<int64_t>(source.s32X) * config_.stream_width / detect_width);
            target.y = static_cast<int32_t>(
                static_cast<int64_t>(source.s32Y) * config_.stream_height / detect_height);
            target.width = static_cast<uint32_t>(
                static_cast<uint64_t>(source.u32Width) * config_.stream_width /
                detect_width);
            target.height = static_cast<uint32_t>(
                static_cast<uint64_t>(source.u32Height) * config_.stream_height /
                detect_height);
        }

        if (event_channel_ != nullptr) {
            SharedMutexGuard guard(&event_channel_->lock);
            if (guard.locked()) {
                event_channel_->motion = event;
                event_channel_->motion_sequence = ++published_motion_sequence_;
            }
        }

        if (event.motion_present) {
            RK_LOGD("Motion: frame %u, %u rect(s), area %u/%u", event.frame_id,
                    event.rectangle_count, event.moving_pixel_area,
                    event.frame_pixel_area);
        }
    }

    RK_MPI_IVS_ReleaseResults(kIvsChannel, &results);
}

void MediaPipeline::PublishHeartbeat() {
    if (health_channel_ == nullptr) {
        return;
    }

    SharedMutexGuard guard(&health_channel_->lock);
    if (!guard.locked()) {
        return;
    }

    ProcessHeartbeat& beat =
        health_channel_->beats[static_cast<uint32_t>(MonitoredProcess::MEDIA)];
    beat.counter = ++heartbeat_counter_;
    beat.updated_at_us = MonotonicMicroseconds();
}

int MediaPipeline::Run() {
    signal(SIGTERM, HandleStopSignal);
    signal(SIGINT, HandleStopSignal);

    RK_LOGI("Media pipeline running");

    uint64_t last_heartbeat_us = 0;
    constexpr uint64_t kHeartbeatIntervalUs = 1000000;

    while (g_stop_requested == 0) {
        ForwardEncodedFrame();
        PublishMotionResults();

        const uint64_t now_us = MonotonicMicroseconds();
        if (now_us - last_heartbeat_us >= kHeartbeatIntervalUs) {
            PublishHeartbeat();
            last_heartbeat_us = now_us;
        }

        // No sleep here on purpose. RK_MPI_VENC_GetStream blocks for up to 20ms
        // when no frame is ready, which paces the loop and yields the core.
        // An unconditional usleep would add latency to every frame.
    }

    RK_LOGI("Media pipeline stopping");
    return 0;
}

void MediaPipeline::TeardownBindings() {
    // Unbind in reverse order of binding: a downstream module must stop
    // receiving before its producer is torn down.
    if (bound_vpss_to_ivs_) {
        MPP_CHN_S source{RK_ID_VPSS, kVpssGroup, kVpssDetectChannel};
        MPP_CHN_S destination{RK_ID_IVS, 0, kIvsChannel};
        RK_MPI_SYS_UnBind(&source, &destination);
        bound_vpss_to_ivs_ = false;
    }

    if (bound_vpss_to_venc_) {
        MPP_CHN_S source{RK_ID_VPSS, kVpssGroup, kVpssEncodeChannel};
        MPP_CHN_S destination{RK_ID_VENC, 0, kVencChannel};
        RK_MPI_SYS_UnBind(&source, &destination);
        bound_vpss_to_venc_ = false;
    }

    if (bound_vi_to_vpss_) {
        MPP_CHN_S source{RK_ID_VI, kViDevice, kViChannel};
        MPP_CHN_S destination{RK_ID_VPSS, kVpssGroup, kVpssEncodeChannel};
        RK_MPI_SYS_UnBind(&source, &destination);
        bound_vi_to_vpss_ = false;
    }
}

void MediaPipeline::TeardownModules() {
    if (rtsp_session_ != nullptr) {
        rtsp_del_session(rtsp_session_);
        rtsp_session_ = nullptr;
    }
    if (rtsp_server_ != nullptr) {
        rtsp_del_demo(rtsp_server_);
        rtsp_server_ = nullptr;
    }

    if (motion_detector_ready_) {
        RK_MPI_IVS_DestroyChn(kIvsChannel);
        motion_detector_ready_ = false;
    }

    if (encoder_ready_) {
        RK_MPI_VENC_StopRecvFrame(kVencChannel);
        RK_MPI_VENC_DestroyChn(kVencChannel);
        encoder_ready_ = false;
    }

    if (scaler_ready_) {
        RK_MPI_VPSS_StopGrp(kVpssGroup);
        RK_MPI_VPSS_DisableChn(kVpssGroup, kVpssDetectChannel);
        RK_MPI_VPSS_DisableChn(kVpssGroup, kVpssEncodeChannel);
        RK_MPI_VPSS_DestroyGrp(kVpssGroup);
        scaler_ready_ = false;
    }

    if (video_input_ready_) {
        RK_MPI_VI_DisableChn(kViDevice, kViChannel);
        RK_MPI_VI_DisableDev(kViDevice);
        video_input_ready_ = false;
    }

    RK_MPI_SYS_Exit();
}

}  // namespace baby_monitor
