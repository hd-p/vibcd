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

// IVS analyses on a macroblock grid, so its input dimensions round up to 16.
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

    // Before VI, not after: the ISP only produces frames while the 3A loop is
    // running. Without this, the sensor filled the VI buffers once (exactly
    // u32BufCount frames) and then stopped forever, which looked like a broken
    // encoder rather than a missing AE/AWB engine.
    //
    // An empty iq_file_dir skips 3A entirely rather than failing. That is a
    // diagnostic escape hatch: simple_vi_bind_vpss_bind_venc.c encodes happily
    // on this board without ever starting 3A, so being able to run the same way
    // isolates whether our 3A setup is what starves the encoder. Exposure will
    // be whatever the sensor defaults to.
    if (!config_.iq_file_dir.empty()) {
        if (!isp_.Start(kViDevice, config_.iq_file_dir)) {
            RK_LOGE("ISP/3A engine did not start; the sensor will not deliver frames");
            return false;
        }
    } else {
        RK_LOGW("ISP 3A disabled (empty iqfiles path); exposure is uncontrolled");
    }

    // Order matters: every module must exist before anything is bound, because
    // binding starts data flowing immediately.
    if (!InitialiseVideoInput()) return false;
    if (config_.enable_motion_detection && !InitialiseDetectInput()) return false;
    if (!InitialiseScaler()) return false;
    if (!InitialiseEncoder()) return false;
    if (config_.enable_motion_detection && !InitialiseMotionDetector()) return false;
    if (!InitialiseRtspServer()) return false;
    if (!BindPipeline()) return false;

    if (config_.enable_motion_detection) {
        RK_LOGI("Media pipeline ready: VI ch0 %ux%u -> VPSS -> VENC %ux%u, "
                "VI ch1 %ux%u -> IVS",
                config_.sensor_width, config_.sensor_height, config_.stream_width,
                config_.stream_height, config_.detect_width, config_.detect_height);
    } else {
        RK_LOGI("Media pipeline ready: VI %ux%u -> VPSS -> VENC %ux%u "
                "(motion detection disabled)",
                config_.sensor_width, config_.sensor_height, config_.stream_width,
                config_.stream_height);
    }
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

    // 2, matching simple_vi_bind_vpss_bind_venc.c, which encodes fine on this
    // board. Raising it to 3 was an attempt to give the ISP more headroom and it
    // did not help.
    channel_attributes.stIspOpt.u32BufCount = 2;
    channel_attributes.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    channel_attributes.stSize.u32Width = config_.sensor_width;
    channel_attributes.stSize.u32Height = config_.sensor_height;
    channel_attributes.enPixelFormat = RK_FMT_YUV420SP;
    channel_attributes.enCompressMode = COMPRESS_MODE_NONE;

    // stFrameRate is deliberately left zeroed. The driver logs "illegal param
    // s32SrcFrameRate(0) s32DstFrameRate(0)" for this, and setting -1/-1 to
    // silence that warning is what the working sample does NOT do - it leaves
    // the field at zero and encodes at the sensor rate regardless. The warning
    // is cosmetic; matching the sample matters more.

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

bool MediaPipeline::InitialiseDetectInput() {
    // A second channel on the same VI device and pipe. The ISP scales this one
    // down on its own, so the detection path costs no RGA time at all - that is
    // the whole point of fanning out here rather than at VPSS. rkipc runs three
    // such channels (2560x1440, 704x576, 960x540) off one pipe, which is what
    // establishes that per-channel sizes are independent.
    VI_CHN_ATTR_S channel_attributes;
    memset(&channel_attributes, 0, sizeof(channel_attributes));

    channel_attributes.stIspOpt.u32BufCount = 2;
    channel_attributes.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;

    // stMaxSize bounds the buffers the ISP allocates for this channel; stSize is
    // what it actually delivers. Both are the detection resolution here, so the
    // channel costs about 345 KB per buffer instead of a 1080p frame's 3 MB.
    channel_attributes.stIspOpt.stMaxSize.u32Width = AlignUpTo16(config_.detect_width);
    channel_attributes.stIspOpt.stMaxSize.u32Height = AlignUpTo16(config_.detect_height);
    channel_attributes.stSize.u32Width = AlignUpTo16(config_.detect_width);
    channel_attributes.stSize.u32Height = AlignUpTo16(config_.detect_height);
    channel_attributes.enPixelFormat = RK_FMT_YUV420SP;
    channel_attributes.enCompressMode = COMPRESS_MODE_NONE;

    // Depth 0 for the same reason as channel 0: IVS is a bound consumer, so no
    // userspace queue should accumulate.
    channel_attributes.u32Depth = 0;

    RK_S32 result =
        RK_MPI_VI_SetChnAttr(kViDevice, kViDetectChannel, &channel_attributes);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr(detect) failed: %#x", result);
        return false;
    }

    result = RK_MPI_VI_EnableChn(kViDevice, kViDetectChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_EnableChn(detect) failed: %#x", result);
        return false;
    }

    detect_input_ready_ = true;
    return true;
}

bool MediaPipeline::InitialiseScaler() {
    VPSS_GRP_ATTR_S group_attributes;
    memset(&group_attributes, 0, sizeof(group_attributes));

    // 4096x4096, not the sensor size. This is the group's upper bound, not its
    // working resolution, and every SDK sample hardcodes the maximum here. A
    // group sized exactly to the sensor left no headroom for the channel
    // scaler's internal alignment.
    group_attributes.u32MaxW = 4096;
    group_attributes.u32MaxH = 4096;
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

    // Deliberately NOT calling SetVProcDev / ResetGrp / SetGrpCrop here. Those
    // appear in the SDK's larger helper (SAMPLE_COMM_VPSS_CreateChn) but not in
    // simple_vi_bind_vpss_bind_venc.c, which was verified working on this exact
    // board: it goes straight from CreateGrp to SetChnAttr. Adding them did not
    // fix the starvation and they are extra state to get wrong, so this follows
    // the minimal sequence that is known to produce frames here.

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
    // userspace queue should accumulate. Raising this to 1 was tried and made
    // things worse - zero frames instead of one, "mpp: select failed!", and RGA
    // interrupts dropping from ~40/s to ~11/s. The "0, get fail" note in the SDK
    // sample refers to userspace RK_MPI_*_GetChnFrame, not to bound delivery.
    encode_channel.u32Depth = 0;

    result = RK_MPI_VPSS_SetChnAttr(kVpssGroup, kVpssEncodeChannel, &encode_channel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_SetChnAttr(encode) failed: %#x", result);
        return false;
    }
    // Enable VPSS encode channel BEFORE binding (SDK sample order)
    result = RK_MPI_VPSS_EnableChn(kVpssGroup, kVpssEncodeChannel);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VPSS_EnableChn(encode) failed: %#x", result);
        return false;
    }

    // Single-channel group on purpose. The detection path used to be VPSS
    // channel 1, but adding it starved the encoder here, and IVS now takes its
    // frames from VI channel 1 instead, so nothing else needs a VPSS channel.

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
    // Verbatim, no alignment. simple_vi_bind_vpss_bind_venc.c passes width and
    // height straight through here and encodes fine on this board; align-16 was
    // what caused the earlier "frame info no equal set drop" flood, and align-2
    // is simply unnecessary for the even resolutions we use.
    encoder_attributes.stVencAttr.u32VirWidth = config_.stream_width;
    encoder_attributes.stVencAttr.u32VirHeight = config_.stream_height;
    encoder_attributes.stVencAttr.u32StreamBufCnt = 2;

    // Full raw-frame size, matching every working SDK sample. The earlier
    // w*h/2 was a bandwidth-saving guess: a single I frame at 1080p can exceed
    // it, and an undersized stream buffer is one of the ways this pipeline
    // silently delivers nothing instead of reporting an error.
    encoder_attributes.stVencAttr.u32BufSize =
        config_.stream_width * config_.stream_height * 3 / 2;

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

    // simple_vi_bind_vpss_bind_venc.c does not call SetRcParam and encodes
    // successfully. Omitting it to match the minimal working reference exactly;
    // the QP ranges in stRcAttr.stH264Cbr / stH265Cbr are sufficient, and
    // SetRcParam may trigger driver race conditions under certain bind topologies.

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

    // The input is now the full sensor rate (~30fps) straight off VI channel 1,
    // so this interval carries the decimation that VPSS's 30->10fps channel used
    // to do: every 6th frame is ~5 analyses per second, the same effective rate
    // as before, plenty for a sleeping infant.
    //
    // The decimation is done here rather than on the VI channel because
    // VI_CHN_ATTR_S::stFrameRate is not usable for it on this board - the driver
    // rejects the field (see the note in InitialiseVideoInput), and rkipc does
    // not set it on VI channels either.
    detector_attributes.s32MDInterval = 6;
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

    if (config_.enable_motion_detection) {
        // Straight from the second VI channel, bypassing VPSS entirely: the ISP
        // has already scaled this channel to detection resolution, so IVS needs
        // no RGA pass and the encode path keeps the VPSS group to itself.
        MPP_CHN_S detect_input_channel;
        detect_input_channel.enModId = RK_ID_VI;
        detect_input_channel.s32DevId = kViDevice;
        detect_input_channel.s32ChnId = kViDetectChannel;

        MPP_CHN_S detector_channel;
        detector_channel.enModId = RK_ID_IVS;
        detector_channel.s32DevId = 0;
        detector_channel.s32ChnId = kIvsChannel;

        result = RK_MPI_SYS_Bind(&detect_input_channel, &detector_channel);
        if (result != RK_SUCCESS) {
            RK_LOGE("Bind VI -> IVS failed: %#x", result);
            return false;
        }
        bound_vi_to_ivs_ = true;
    }

    return true;
}

bool MediaPipeline::ForwardEncodedFrame() {
    VENC_STREAM_S encoded_stream;
    memset(&encoded_stream, 0, sizeof(encoded_stream));

    VENC_PACK_S stream_packet;
    memset(&stream_packet, 0, sizeof(stream_packet));
    encoded_stream.pstPack = &stream_packet;

    // 200 ms, matching the SDK's RTSP samples. The previous 20 ms was shorter
    // than the 33 ms frame interval, so the call timed out and returned
    // BUF_EMPTY before the encoder had any frame ready - the loop spun without
    // ever collecting one. No SDK sample uses a timeout below 500; most block
    // outright with -1. This is still bounded so the loop can service RTSP
    // events and the heartbeat.
    RK_S32 result = RK_MPI_VENC_GetStream(kVencChannel, &encoded_stream, 200000);
    if (result != RK_SUCCESS) {
        // An empty queue is routine. Anything else is remembered rather than
        // logged here, because this runs up to 50 times a second and a genuine
        // fault would drown the log; Run() reports it with the frame stats.
        if (result != RK_ERR_VENC_BUF_EMPTY) {
            last_getstream_error_ = result;
        }
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
            ++frames_forwarded_;
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

    // Long enough that the log stays readable over hours, short enough that a
    // stalled video path is obvious while someone is still watching.
    constexpr uint64_t kStatsIntervalUs = 10ULL * 1000 * 1000;

    uint64_t last_report_us = MonotonicMicroseconds();
    uint64_t frames_at_last_report = 0;

    while (g_stop_requested == 0) {
        ForwardEncodedFrame();
        if (config_.enable_motion_detection) {
            PublishMotionResults();
        }

        // Unconditionally, not just after a frame: this is what services the
        // RTSP control channel (OPTIONS/DESCRIBE/SETUP/PLAY). Calling it only
        // when a frame arrives means a client cannot even complete the handshake
        // while the encoder is starved, which leaves its connection sitting in
        // CLOSE_WAIT and makes a stalled encoder look like a broken server.
        if (rtsp_server_ != nullptr) {
            rtsp_do_event(rtsp_server_);
        }

        const uint64_t now_us = MonotonicMicroseconds();
        if (now_us - last_heartbeat_us >= kHeartbeatIntervalUs) {
            PublishHeartbeat();
            last_heartbeat_us = now_us;
        }

        // Periodic proof of life for the video path. Without this, "no video"
        // and "no client" are indistinguishable from the outside.
        if (now_us - last_report_us >= kStatsIntervalUs) {
            const uint64_t elapsed_us = now_us - last_report_us;
            const uint64_t frames = frames_forwarded_ - frames_at_last_report;
            const unsigned fps =
                static_cast<unsigned>(frames * 1000000 / (elapsed_us ? elapsed_us : 1));

            if (frames == 0) {
                RK_LOGW("No encoded frames in the last %llu s (VENC GetStream "
                        "last error %#x); check that VI is streaming",
                        static_cast<unsigned long long>(elapsed_us / 1000000),
                        last_getstream_error_);
            } else {
                RK_LOGI("Video: %llu frames forwarded, ~%u fps",
                        static_cast<unsigned long long>(frames_forwarded_), fps);
            }

            last_report_us = now_us;
            frames_at_last_report = frames_forwarded_;
        }

        // No sleep here on purpose. RK_MPI_VENC_GetStream blocks until a frame
        // is ready (or 200 ms elapse), which paces the loop and yields the core.
        // An unconditional usleep would add latency to every frame.
    }

    RK_LOGI("Media pipeline stopping");
    return 0;
}

void MediaPipeline::TeardownBindings() {
    // Unbind in reverse order of binding: a downstream module must stop
    // receiving before its producer is torn down.
    if (bound_vi_to_ivs_) {
        MPP_CHN_S source{RK_ID_VI, kViDevice, kViDetectChannel};
        MPP_CHN_S destination{RK_ID_IVS, 0, kIvsChannel};
        RK_MPI_SYS_UnBind(&source, &destination);
        bound_vi_to_ivs_ = false;
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
        RK_MPI_VPSS_DisableChn(kVpssGroup, kVpssEncodeChannel);
        RK_MPI_VPSS_DestroyGrp(kVpssGroup);
        scaler_ready_ = false;
    }

    // Before the video_input_ready_ block, not after: that one calls
    // RK_MPI_VI_DisableDev, and disabling the device while channel 1 is still
    // enabled leaves the driver holding a channel on a dead device.
    if (detect_input_ready_) {
        RK_MPI_VI_DisableChn(kViDevice, kViDetectChannel);
        detect_input_ready_ = false;
    }

    if (video_input_ready_) {
        RK_MPI_VI_DisableChn(kViDevice, kViChannel);
        RK_MPI_VI_DisableDev(kViDevice);
        video_input_ready_ = false;
    }

    // After VI, mirroring the startup order. Stopping 3A while VI still holds
    // the ISP streaming leaves the driver programming a sensor nobody is
    // reading, and the next run then finds the hardware in a half-claimed state.
    isp_.Stop();

    RK_MPI_SYS_Exit();
}

}  // namespace baby_monitor
