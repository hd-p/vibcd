#include "app/supervisor.h"

#include <csignal>
#include <ctime>
#include <utility>

#include "app/audio_service.h"
#include "app/hardware_watchdog.h"
#include "app/media_service.h"
#include "base/child_process.h"
#include "base/plain_log.h"
#include "base/rk_platform.h"
#include "base/shared_memory.h"
#include "base/shared_records.h"

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

void SleepMilliseconds(uint32_t milliseconds) {
    struct timespec interval;
    interval.tv_sec = milliseconds / 1000;
    interval.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&interval, nullptr);
}

// One worker plus the heartbeat the supervisor expects it to keep advancing.
struct SupervisedWorker {
    ChildProcess process;
    MonitoredProcess heartbeat_slot;
    uint64_t last_observed_counter = 0;
    uint64_t last_progress_us = 0;
    bool abandoned = false;

    SupervisedWorker(std::string name, ChildEntryPoint entry_point,
                     MonitoredProcess slot)
        : process(std::move(name), std::move(entry_point)), heartbeat_slot(slot) {}
};

// Returns false when the lock could not be taken. The caller must not treat
// that as a stall: failing to read a heartbeat is not evidence that the worker
// stopped producing one.
bool ReadHeartbeatCounter(HealthChannel* channel, MonitoredProcess slot,
                          uint64_t* counter) {
    SharedMutexGuard guard(&channel->lock);
    if (!guard.locked()) {
        return false;
    }
    *counter = channel->beats[static_cast<uint32_t>(slot)].counter;
    return true;
}

// Clears a slot so a restarted worker starts from a known state instead of
// inheriting the dead instance's counter, which would look like instant progress.
void ResetHeartbeat(HealthChannel* channel, MonitoredProcess slot) {
    SharedMutexGuard guard(&channel->lock);
    if (!guard.locked()) {
        return;
    }
    ProcessHeartbeat& beat = channel->beats[static_cast<uint32_t>(slot)];
    beat.counter = 0;
    beat.updated_at_us = 0;
}

// Creates both shared channels and initialises their mutexes. Shared by the
// supervisor and the single-service debug path, which must produce identical
// channels: a worker only ever attaches, never creates.
bool CreateChannels(SharedMemory<EventChannel>* event_channel,
                    SharedMemory<HealthChannel>* health_channel) {
    // Clear debris from an unclean shutdown before creating fresh segments. A
    // leftover segment can be the wrong size for the current build, or hold a
    // mutex locked by a process that no longer exists.
    SharedMemory<EventChannel>::Unlink(kEventChannelName);
    SharedMemory<HealthChannel>::Unlink(kHealthChannelName);

    if (!event_channel->Create(kEventChannelName)) {
        PLAIN_LOGE("Could not create %s", kEventChannelName);
        return false;
    }
    if (!InitialiseSharedMutex(&(*event_channel)->lock)) {
        PLAIN_LOGE("Could not initialise the event channel mutex");
        return false;
    }

    if (!health_channel->Create(kHealthChannelName)) {
        PLAIN_LOGE("Could not create %s", kHealthChannelName);
        return false;
    }
    if (!InitialiseSharedMutex(&(*health_channel)->lock)) {
        PLAIN_LOGE("Could not initialise the health channel mutex");
        return false;
    }

    PLAIN_LOGI("Shared channels ready: events %zu bytes, health %zu bytes",
            sizeof(EventChannel), sizeof(HealthChannel));
    return true;
}

}  // namespace

int RunSingleService(const SupervisorConfig& config, MonitoredProcess service) {
    // No fork, no restarts, and deliberately no hardware watchdog: this path
    // exists to be stepped through in a debugger, and a breakpoint held longer
    // than the watchdog timeout would reset the board.
    SharedMemory<EventChannel> event_channel;
    SharedMemory<HealthChannel> health_channel;
    if (!CreateChannels(&event_channel, &health_channel)) {
        return 1;
    }

    switch (service) {
        case MonitoredProcess::MEDIA:
            PLAIN_LOGI("Running baby_media in this process (no fork, no watchdog)");
            return RunMediaService(config.media);
        case MonitoredProcess::AUDIO:
            PLAIN_LOGI("Running baby_audio in this process (no fork, no watchdog)");
            return RunAudioService(config.audio);
        default:
            PLAIN_LOGE("Not a runnable service");
            return 1;
    }
}

int RunSupervisor(const SupervisorConfig& config) {
    signal(SIGTERM, HandleStopSignal);
    signal(SIGINT, HandleStopSignal);

    // Children are reaped explicitly through waitpid, so SIGCHLD must stay at
    // its default disposition. Setting it to SIG_IGN would make the kernel
    // auto-reap, and waitpid would then never report an exit status.
    signal(SIGCHLD, SIG_DFL);

    SharedMemory<EventChannel> event_channel;
    SharedMemory<HealthChannel> health_channel;
    if (!CreateChannels(&event_channel, &health_channel)) {
        return 1;
    }

    // Copied into the lambdas so each child runs from its own snapshot and does
    // not depend on the parent's frame still being alive.
    const MediaPipelineConfig media_config = config.media;
    const AudioPipelineConfig audio_config = config.audio;

    // A plain array, not a vector: ChildProcess deliberately forbids copying and
    // moving, because two objects believing they own the same pid is a bug
    // waiting to happen. Array elements are constructed in place, so no move is
    // ever required, and the worker set is fixed at build time anyway.
    SupervisedWorker workers[] = {
        {"baby_media", [media_config]() { return RunMediaService(media_config); },
         MonitoredProcess::MEDIA},
        {"baby_audio", [audio_config]() { return RunAudioService(audio_config); },
         MonitoredProcess::AUDIO},
    };

    const uint64_t startup_us = MonotonicMicroseconds();
    for (SupervisedWorker& worker : workers) {
        worker.last_progress_us = startup_us;
        if (!worker.process.Start()) {
            PLAIN_LOGE("Could not start %s", worker.process.name().c_str());
        }
    }

    HardwareWatchdog watchdog;
    watchdog.Open(config.watchdog_device,
                  static_cast<int>(config.watchdog_timeout_seconds));

    while (g_stop_requested == 0) {
        SleepMilliseconds(config.poll_interval_ms);
        const uint64_t now_us = MonotonicMicroseconds();

        for (SupervisedWorker& worker : workers) {
            if (worker.abandoned) {
                continue;
            }

            // Reaps as a side effect, so an exited child never lingers as a
            // zombie and the liveness answer below is accurate. Note that
            // kill(pid, 0) would be the wrong test: signalling a zombie
            // succeeds, so it reports dead processes as healthy.
            const bool running = worker.process.PollLiveness();
            bool restart_needed = false;

            if (!running) {
                PLAIN_LOGE("%s exited with status %d", worker.process.name().c_str(),
                        worker.process.last_exit_status());
                restart_needed = true;
            } else {
                uint64_t observed_counter = 0;
                if (ReadHeartbeatCounter(health_channel.get(), worker.heartbeat_slot,
                                         &observed_counter)) {
                    if (observed_counter != worker.last_observed_counter) {
                        worker.last_observed_counter = observed_counter;
                        worker.last_progress_us = now_us;
                    } else if (now_us - worker.last_progress_us >
                               config.heartbeat_timeout_us) {
                        // The process exists but its loop stopped advancing.
                        // This is precisely the failure a pid check cannot see.
                        PLAIN_LOGE("%s is alive but its heartbeat stalled for %llu ms",
                                worker.process.name().c_str(),
                                static_cast<unsigned long long>(
                                    (now_us - worker.last_progress_us) / 1000));
                        worker.process.Stop();
                        restart_needed = true;
                    }
                }
            }

            if (!restart_needed) {
                continue;
            }

            if (worker.process.restart_count() >= config.max_restarts) {
                PLAIN_LOGE("%s failed %d times; giving up on it",
                        worker.process.name().c_str(), worker.process.restart_count());
                worker.abandoned = true;
                continue;
            }

            ResetHeartbeat(health_channel.get(), worker.heartbeat_slot);
            worker.process.RecordRestart();
            worker.last_observed_counter = 0;
            worker.last_progress_us = now_us;

            if (worker.process.Start()) {
                PLAIN_LOGI("Restarted %s (attempt %d of %d)", worker.process.name().c_str(),
                        worker.process.restart_count(), config.max_restarts);
            } else {
                PLAIN_LOGE("Could not restart %s", worker.process.name().c_str());
            }
        }

        // Fed only after the checks above finish. If this loop stalls, feeding
        // stops and the board reboots, which is the intended outcome for a
        // wedged supervisor.
        watchdog.Feed();
    }

    PLAIN_LOGI("Supervisor stopping workers");

    // Disarm the dog first: stopping the workers takes seconds, and an armed
    // watchdog would reboot the board partway through a deliberate shutdown.
    watchdog.DisarmAndClose();

    for (SupervisedWorker& worker : workers) {
        worker.process.Stop();
    }

    PLAIN_LOGI("Supervisor stopped");
    return 0;
}

}  // namespace baby_monitor
