// Child process lifecycle, owned exclusively by the parent that forked it.
//
// The previous design had a WatchdogProcess forked as a *sibling* of the
// workers, which cannot work: waitpid() from a sibling returns ECHILD, so
// crashed children stayed zombies forever, and on Linux kill(zombie, 0)
// succeeds, so the watchdog concluded a dead process was healthy. Worse, the
// sibling restarted workers into its own address space, so the parent's pid
// bookkeeping silently went stale and shutdown signalled the wrong pids.
//
// Here the parent is the only supervisor. It is the only process that can reap,
// so it is the only one able to tell "exited" from "still running", and its pid
// records are authoritative by construction.

#ifndef BABY_MONITOR_BASE_CHILD_PROCESS_H
#define BABY_MONITOR_BASE_CHILD_PROCESS_H

#include <sys/types.h>

#include <functional>
#include <string>

namespace baby_monitor {

// Work performed inside the forked child. Returns the process exit status.
// Runs after the fork, so it must not rely on threads or locks held by the
// parent at fork time.
using ChildEntryPoint = std::function<int()>;

enum class ChildState {
    NEVER_STARTED,
    RUNNING,
    EXITED,
};

class ChildProcess {
public:
    ChildProcess(std::string name, ChildEntryPoint entry_point);
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool Start();

    // Reaps the child without blocking and updates state. Must be called
    // periodically by the parent, both to notice exits and to stop zombies
    // accumulating. Returns true if the child is still running.
    bool PollLiveness();

    // SIGTERM, wait up to the grace period, then SIGKILL. Always reaps.
    void Stop(int grace_period_ms = 3000);

    const std::string& name() const { return name_; }
    pid_t pid() const { return pid_; }
    ChildState state() const { return state_; }
    int restart_count() const { return restart_count_; }
    void RecordRestart() { ++restart_count_; }

    // Exit status of the most recent run, meaningful once state() is EXITED.
    // Negative means the child was killed by that signal number.
    int last_exit_status() const { return last_exit_status_; }

private:
    void ReapInto(int wait_status);

    std::string name_;
    ChildEntryPoint entry_point_;
    pid_t pid_ = -1;
    ChildState state_ = ChildState::NEVER_STARTED;
    int restart_count_ = 0;
    int last_exit_status_ = 0;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_BASE_CHILD_PROCESS_H
