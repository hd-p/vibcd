// Hardware watchdog wrapper.
//
// /dev/watchdog reboots the board unless it is written to within the configured
// timeout. This is the last line of defence: if the supervisor itself hangs or
// is killed, nothing feeds the device and the board restarts. Nothing in user
// space can rescue a broken supervisor, so there has to be something below it.
//
// Two conventions in this driver interface are easy to get wrong:
//
//   Magic close - writing 'V' before close() is the explicit "I meant to
//                 disarm this" signal. Without it, drivers built with
//                 CONFIG_WATCHDOG_NOWAYOUT keep counting down after close.
//                 So the magic write belongs only on the intentional shutdown
//                 path, never in the destructor: an object destroyed by an
//                 unexpected unwind should leave the timer armed, because a
//                 board that reboots itself is better than one that has
//                 silently lost its watchdog.
//
//   Clamped     - drivers clamp the requested timeout to what the silicon
//     timeout     supports, so the value actually in force must be read back
//                 rather than assumed when choosing a feed interval.

#ifndef BABY_MONITOR_APP_HARDWARE_WATCHDOG_H
#define BABY_MONITOR_APP_HARDWARE_WATCHDOG_H

#include <string>

namespace baby_monitor {

class HardwareWatchdog {
public:
    HardwareWatchdog() = default;
    ~HardwareWatchdog();

    HardwareWatchdog(const HardwareWatchdog&) = delete;
    HardwareWatchdog& operator=(const HardwareWatchdog&) = delete;

    // Opens and arms the device. An empty path disables hardware supervision,
    // which is what you want under a debugger, where a breakpoint would
    // otherwise stop the feeding and reboot the board.
    //
    // Returns false when unavailable. Callers should treat that as degraded
    // rather than fatal: software supervision still works without the timer.
    bool Open(const std::string& device_path, int requested_timeout_seconds);

    // Postpones the reset. Must be called well inside the effective timeout.
    void Feed();

    // Disarms via magic close. Only for a deliberate, orderly shutdown.
    void DisarmAndClose();

    bool is_open() const { return descriptor_ >= 0; }
    int effective_timeout_seconds() const { return effective_timeout_seconds_; }

private:
    int descriptor_ = -1;
    int effective_timeout_seconds_ = 0;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_APP_HARDWARE_WATCHDOG_H
