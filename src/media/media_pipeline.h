// Media pipeline: VI ch0 -> VENC -> RTSP, and VI ch1 -> IVS -> motion
// rectangles. The fan-out is at VI: two ISP outputs on one device, each sized
// by the ISP itself, so neither branch costs any scaler time and the detection
// path never competes with the encoder.
//
// There is no VPSS anywhere in here. The RV1106 has no dedicated VPSS block -
// scaling runs on RGA - so a group whose input and output are both
// stream resolution was a pure 1:1 copy: ~186 MB/s of RGA traffic at 1080p30 to
// reproduce the frame it was handed. Both vendor references for this topology
// (simple_vi_bind_venc_rtsp.c and rkipc's rv1106_ipc) bind VI straight to VENC
// and let the ISP do whatever scaling each consumer needs.
//
// Bound with RK_MPI_SYS_Bind, so pixels move between hardware blocks without
// ever entering this process's address space. The CPU only handles encoded
// bitstream (a few hundred KB/s) and motion rectangles (bytes).
//
// This is why the whole video chain lives in a single process. Bound channel
// handles are owned by the calling process, so splitting VENC and IVS into
// separate processes would force pixels back through shared memory. On a single
// core there is nothing to gain by splitting: processes time-slice either way,
// and the split would only add copies and context switches.

#ifndef BABY_MONITOR_MEDIA_MEDIA_PIPELINE_H
#define BABY_MONITOR_MEDIA_MEDIA_PIPELINE_H

#include <cstdint>
#include <string>

#include "base/rk_platform.h"
#include "base/shared_records.h"
#include "media/isp_controller.h"

namespace baby_monitor {

struct MediaPipelineConfig {
    uint32_t sensor_width = 1920;
    uint32_t sensor_height = 1080;

    // Encoder resolution, and with it the size VI channel 0 is configured to
    // deliver. The ISP scales into this, so it may differ from the sensor size.
    uint32_t stream_width = 1920;
    uint32_t stream_height = 1080;

    // Motion detection runs on a downscaled channel: IVS cost scales with area
    // and rectangle coordinates are trivially rescaled afterwards.
    uint32_t detect_width = 640;
    uint32_t detect_height = 360;

    uint32_t bitrate_kbps = 2048;
    bool use_h265 = false;
    int rtsp_port = 554;
    std::string rtsp_path = "/live/0";

    // Motion detection can be switched off entirely, which drops the second
    // VI channel and the IVS binding with it. Useful for isolating the video
    // path: if the encoder only produces frames with this disabled, the fault
    // is in the fan-out rather than in the encode chain itself.
    bool enable_motion_detection = true;

    // IVS motion sensitivity, 1 = low, 2 = medium, 3 = high.
    uint32_t motion_sensitivity = 2;

    // Motion is reported when the moving area exceeds this fraction of the
    // frame, expressed in per-mille to avoid floating point in the hot path.
    uint32_t motion_area_threshold_permille = 20;

    // Sensor tuning files for the ISP 3A loop. Without a running 3A loop the
    // ISP delivers only as many frames as the VI buffer count and then stops,
    // so this is required rather than optional. /etc/iqfiles is a symlink to
    // /oem/usr/share/iqfiles on this board.
    std::string iq_file_dir = "/etc/iqfiles";
};

class MediaPipeline {
public:
    MediaPipeline(const MediaPipelineConfig& config, EventChannel* event_channel,
                  HealthChannel* health_channel);
    ~MediaPipeline();

    MediaPipeline(const MediaPipeline&) = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;

    bool Initialise();

    // Runs until the process receives SIGTERM/SIGINT. Returns a process exit
    // status.
    int Run();

private:
    bool InitialiseVideoInput();

    // Second ISP output on the same VI device, scaled to detection resolution
    // and bound straight to IVS. Only called when motion detection is enabled.
    bool InitialiseDetectInput();

    bool InitialiseEncoder();
    bool InitialiseMotionDetector();
    bool InitialiseRtspServer();
    bool BindPipeline();

    void TeardownBindings();
    void TeardownModules();

    // Moves one encoded frame from VENC to the RTSP session. Returns false only
    // on an error worth logging; an empty queue is a normal outcome.
    bool ForwardEncodedFrame();

    // Drains IVS results and republishes a verdict to shared memory.
    void PublishMotionResults();

    void PublishHeartbeat();

    MediaPipelineConfig config_;
    EventChannel* event_channel_;
    HealthChannel* health_channel_;

    // Fixed channel assignments. VI channel 0 delivers stream resolution
    // straight to the encoder; VI channel 1 is a second ISP output, scaled
    // down, that feeds IVS. Both are plain ISP outputs on one pipe, which is
    // how rkipc drives its three channels too.
    static constexpr int kViDevice = 0;
    static constexpr int kViPipe = 0;
    static constexpr int kViChannel = 0;
    static constexpr int kViDetectChannel = 1;
    static constexpr int kVencChannel = 0;
    static constexpr int kIvsChannel = 0;

    bool video_input_ready_ = false;
    bool detect_input_ready_ = false;
    bool encoder_ready_ = false;
    bool motion_detector_ready_ = false;
    bool bound_vi_to_venc_ = false;
    bool bound_vi_to_ivs_ = false;

    // Declared before the MPI state so it is destroyed last: the 3A loop must
    // outlive the VI channel it feeds.
    IspController isp_;

    rtsp_demo_handle rtsp_server_ = nullptr;
    rtsp_session_handle rtsp_session_ = nullptr;

    uint64_t published_motion_sequence_ = 0;
    uint64_t heartbeat_counter_ = 0;

    // Frames handed to the RTSP session since start, and the last non-empty
    // GetStream error. Both exist so Run() can report throughput periodically
    // instead of logging per frame: "no video" and "video but no client" look
    // identical from outside otherwise.
    uint64_t frames_forwarded_ = 0;
    uint64_t frames_dropped_ = 0;
    int32_t last_getstream_error_ = 0;

    // Same idea for the detection path: GetResults failures are silent per
    // call, so the periodic report is the only place they become visible.
    uint64_t ivs_results_ = 0;
    uint64_t ivs_motion_results_ = 0;
    int32_t last_ivs_error_ = 0;
    bool last_motion_present_ = false;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_MEDIA_MEDIA_PIPELINE_H
