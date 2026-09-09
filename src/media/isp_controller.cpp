#include "media/isp_controller.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// These headers declare themselves extern "C" through RKAIQ_BEGIN_DECLARE, so
// unlike the MPI headers they must not be wrapped again. That is why they are
// included here rather than in base/rk_platform.h.
#include "rk_aiq_user_api2_sysctl.h"

#include "base/rk_platform.h"

namespace baby_monitor {
namespace {

// The AIQ error callback runs on a librkaiq thread. XCAM_RETURN_BYPASS is the
// code the library uses to say the pipeline has gone away, which the SDK
// samples treat as "quit". We only log: the supervisor already notices a
// wedged media worker through its heartbeat, and tearing the ISP down from a
// library thread would race with the pipeline shutdown path.
XCamReturn HandleIspError(rk_aiq_err_msg_t* message) {
    if (message != nullptr) {
        RK_LOGE("ISP 3A reported error code %d", message->err_code);
    }
    return XCAM_RETURN_NO_ERROR;
}

}  // namespace

IspController::~IspController() { Stop(); }

bool IspController::Start(int camera_id, const std::string& iq_file_dir) {
    if (context_ != nullptr) {
        return true;
    }

    if (iq_file_dir.empty()) {
        RK_LOGE("ISP 3A needs an iqfiles directory; refusing to start blind");
        return false;
    }

    // Must be set before init, not after: librkaiq reads it while building the
    // pipeline. The SDK samples carry the same comment.
    setenv("HDR_MODE", "0", 1);

    // Ask the library which sensor is actually wired up rather than hardcoding a
    // name. On this board it answers "m00_b_sc3336 4-0030", and the matching
    // tuning file is sc3336_CMK-OT2119-PC1_30IRC-F16.json.
    rk_aiq_static_info_t sensor_static_info;
    memset(&sensor_static_info, 0, sizeof(sensor_static_info));

    XCamReturn enumerate_result =
        rk_aiq_uapi2_sysctl_enumStaticMetas(camera_id, &sensor_static_info);
    if (enumerate_result != XCAM_RETURN_NO_ERROR) {
        RK_LOGE("rk_aiq_uapi2_sysctl_enumStaticMetas(%d) failed: %d", camera_id,
                enumerate_result);
        return false;
    }

    const char* sensor_name = sensor_static_info.sensor_info.sensor_name;
    if (sensor_name == nullptr || sensor_name[0] == '\0') {
        RK_LOGE("No sensor reported for camera %d; is the sensor driver loaded?",
                camera_id);
        return false;
    }

    context_ = rk_aiq_uapi2_sysctl_init(sensor_name, iq_file_dir.c_str(),
                                        HandleIspError, nullptr);
    if (context_ == nullptr) {
        RK_LOGE("rk_aiq_uapi2_sysctl_init(%s, %s) failed; is the tuning file present?",
                sensor_name, iq_file_dir.c_str());
        return false;
    }
    sensor_name_ = sensor_name;

    // Width and height are 0 deliberately: the API takes that as "use the
    // sensor's active output", which it then validates internally. Passing our
    // own stream resolution here would be wrong, since this describes the
    // sensor, not the encoder.
    XCamReturn prepare_result =
        rk_aiq_uapi2_sysctl_prepare(context_, 0, 0, RK_AIQ_WORKING_MODE_NORMAL);
    if (prepare_result != XCAM_RETURN_NO_ERROR) {
        RK_LOGE("rk_aiq_uapi2_sysctl_prepare failed: %d", prepare_result);
        Stop();
        return false;
    }

    XCamReturn start_result = rk_aiq_uapi2_sysctl_start(context_);
    if (start_result != XCAM_RETURN_NO_ERROR) {
        RK_LOGE("rk_aiq_uapi2_sysctl_start failed: %d", start_result);
        Stop();
        return false;
    }
    started_ = true;

    RK_LOGI("ISP 3A running for sensor %s (iqfiles %s)", sensor_name_.c_str(),
            iq_file_dir.c_str());
    return true;
}

void IspController::Stop() {
    if (context_ == nullptr) {
        return;
    }

    // Only stop what was actually started. Calling stop() on a context that
    // never reached the started state is an ordering violation as far as the
    // API is concerned.
    if (started_) {
        // false: let the library restore ircut and similar external hardware,
        // rather than leaving it in whatever state 3A last chose.
        rk_aiq_uapi2_sysctl_stop(context_, false);
        started_ = false;
    }

    rk_aiq_uapi2_sysctl_deinit(context_);
    context_ = nullptr;

    RK_LOGI("ISP 3A stopped");
}

}  // namespace baby_monitor
