// The ISP 3A loop (AE/AWB/AF), driven in-process through librkaiq.
//
// This is not optional on the RV1106. The ISP hardware will fill the VI
// buffers it was given and then stop: without a running 3A loop nothing
// programs the next exposure, so the sensor never produces another frame. The
// symptom is exact and reproducible - VI delivers precisely u32BufCount frames
// (2 in our configuration) and then starves forever, with VENC_GetStream
// returning BUF_EMPTY and no error anywhere. Nothing in the MPI layer reports
// this, because from the MPI point of view nothing failed.
//
// The SDK's own samples all call SAMPLE_COMM_ISP_Init + _Run before touching
// VI, and rkipc (the vendor's own application) does the same. This class is
// that sequence, scoped to one camera.
//
// Lifecycle, which the AIQ API enforces:
//   Start()  ->  init -> prepare -> start
//   Stop()   ->  stop -> deinit
// prepare() cannot be called again after start() without an intervening stop().

#ifndef BABY_MONITOR_MEDIA_ISP_CONTROLLER_H
#define BABY_MONITOR_MEDIA_ISP_CONTROLLER_H

#include <string>

// Opaque here on purpose: the AIQ headers pull in a large tree and declare
// themselves extern "C" via RKAIQ_BEGIN_DECLARE, so they are included only in
// the .cpp rather than leaking into every user of this header.
struct rk_aiq_sys_ctx_s;

namespace baby_monitor {

class IspController {
public:
    IspController() = default;
    ~IspController();

    IspController(const IspController&) = delete;
    IspController& operator=(const IspController&) = delete;

    // Brings up the 3A loop for one camera. iq_file_dir holds the sensor tuning
    // JSON; on this board /etc/iqfiles symlinks to /oem/usr/share/iqfiles and
    // contains sc3336_CMK-OT2119-PC1_30IRC-F16.json for our sensor.
    //
    // Returns false if the loop could not be started. Callers should treat that
    // as fatal for the video path rather than continuing: VI will hand over a
    // couple of frames and then go quiet, which is far harder to diagnose after
    // the fact than a refusal to start.
    bool Start(int camera_id, const std::string& iq_file_dir);

    // Safe to call on a controller that never started, and called from the
    // destructor so a failure part-way through Start() still unwinds.
    void Stop();

    bool running() const { return context_ != nullptr; }

    // The sensor entity name librkaiq matched, e.g. "m00_b_sc3336 4-0030".
    // Empty until Start() succeeds. Worth logging: a mismatch between this and
    // the iq file on disk is the usual reason 3A comes up but exposure is wrong.
    const std::string& sensor_name() const { return sensor_name_; }

private:
    rk_aiq_sys_ctx_s* context_ = nullptr;
    bool started_ = false;
    std::string sensor_name_;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_MEDIA_ISP_CONTROLLER_H
