#include "app/hardware_watchdog.h"

#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "base/rk_platform.h"

namespace baby_monitor {

HardwareWatchdog::~HardwareWatchdog() {
    // No magic close here, deliberately. Reaching the destructor without an
    // explicit DisarmAndClose() means something went wrong, and in that case
    // leaving the timer armed is the safer outcome: the board recovers on its
    // own instead of running on with no watchdog.
    if (descriptor_ >= 0) {
        close(descriptor_);
        descriptor_ = -1;
    }
}

bool HardwareWatchdog::Open(const std::string& device_path,
                            int requested_timeout_seconds) {
    if (device_path.empty()) {
        RK_LOGW("Hardware watchdog disabled by configuration");
        return false;
    }

    descriptor_ = open(device_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (descriptor_ < 0) {
        RK_LOGW("Hardware watchdog unavailable at %s (%s); running with software "
                "supervision only",
                device_path.c_str(), strerror(errno));
        return false;
    }

    int timeout_to_request = requested_timeout_seconds;
    if (ioctl(descriptor_, WDIOC_SETTIMEOUT, &timeout_to_request) != 0) {
        RK_LOGW("WDIOC_SETTIMEOUT(%d) failed: %s", requested_timeout_seconds,
                strerror(errno));
    }

    int queried_timeout = 0;
    if (ioctl(descriptor_, WDIOC_GETTIMEOUT, &queried_timeout) == 0) {
        effective_timeout_seconds_ = queried_timeout;
    } else {
        effective_timeout_seconds_ = requested_timeout_seconds;
    }

    RK_LOGI("Hardware watchdog armed, %d second timeout", effective_timeout_seconds_);
    return true;
}

void HardwareWatchdog::Feed() {
    if (descriptor_ < 0) {
        return;
    }

    if (ioctl(descriptor_, WDIOC_KEEPALIVE, 0) == 0) {
        return;
    }

    // Every watchdog driver treats a write as a keepalive, even those that do
    // not implement the ioctl, so this is a safe fallback rather than a retry.
    if (write(descriptor_, "\0", 1) != 1) {
        RK_LOGE("Could not feed the hardware watchdog: %s", strerror(errno));
    }
}

void HardwareWatchdog::DisarmAndClose() {
    if (descriptor_ < 0) {
        return;
    }

    if (write(descriptor_, "V", 1) != 1) {
        RK_LOGW("Magic close failed; the board may reboot shortly: %s", strerror(errno));
    }

    close(descriptor_);
    descriptor_ = -1;
    RK_LOGI("Hardware watchdog disarmed");
}

}  // namespace baby_monitor
