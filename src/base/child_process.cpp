#include "base/child_process.h"

#include <csignal>
#include <cerrno>
#include <ctime>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include "base/rk_platform.h"

namespace baby_monitor {
namespace {

void SleepMilliseconds(int milliseconds) {
    struct timespec interval;
    interval.tv_sec = milliseconds / 1000;
    interval.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&interval, nullptr);
}

}  // namespace

ChildProcess::ChildProcess(std::string name, ChildEntryPoint entry_point)
    : name_(std::move(name)), entry_point_(std::move(entry_point)) {}

ChildProcess::~ChildProcess() {
    if (state_ == ChildState::RUNNING) {
        Stop();
    }
}

bool ChildProcess::Start() {
    if (state_ == ChildState::RUNNING) {
        return true;
    }

    pid_t forked = fork();
    if (forked < 0) {
        RK_LOGE("fork() for %s failed: %d", name_.c_str(), errno);
        return false;
    }

    if (forked == 0) {
        // Child. Make the name visible in ps/top output, which is how an
        // operator tells these processes apart at a glance.
        prctl(PR_SET_NAME, name_.c_str());

        // The parent's SIGCHLD disposition and any blocked signals are
        // inherited; reset to defaults so the child's own handlers behave
        // predictably.
        signal(SIGCHLD, SIG_DFL);

        int status = entry_point_ ? entry_point_() : 0;

        // _exit, not exit: the child shares the parent's atexit handlers and
        // stdio buffers, and running them twice would double-flush.
        _exit(status);
    }

    pid_ = forked;
    state_ = ChildState::RUNNING;
    RK_LOGI("Started %s as pid %d", name_.c_str(), static_cast<int>(pid_));
    return true;
}

void ChildProcess::ReapInto(int wait_status) {
    state_ = ChildState::EXITED;

    if (WIFEXITED(wait_status)) {
        last_exit_status_ = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        // Negative encodes "killed by signal N", distinguishing a crash from a
        // clean exit with a non-zero status.
        last_exit_status_ = -WTERMSIG(wait_status);
    } else {
        last_exit_status_ = 0;
    }

    pid_ = -1;
}

bool ChildProcess::PollLiveness() {
    if (state_ != ChildState::RUNNING || pid_ <= 0) {
        return false;
    }

    int wait_status = 0;
    pid_t waited = waitpid(pid_, &wait_status, WNOHANG);

    if (waited == 0) {
        // Still running. Note that kill(pid, 0) would be the wrong test here:
        // an exited-but-unreaped child is a zombie, and signalling a zombie
        // succeeds, so that check reports dead processes as alive.
        return true;
    }

    if (waited < 0) {
        if (errno == ECHILD) {
            // Not our child, or already reaped elsewhere. Either way it is gone.
            RK_LOGW("waitpid(%s) reports no such child", name_.c_str());
            state_ = ChildState::EXITED;
            pid_ = -1;
            return false;
        }
        RK_LOGE("waitpid(%s) failed: %d", name_.c_str(), errno);
        return true;
    }

    ReapInto(wait_status);
    return false;
}

void ChildProcess::Stop(int grace_period_ms) {
    if (state_ != ChildState::RUNNING || pid_ <= 0) {
        return;
    }

    const pid_t target = pid_;

    if (kill(target, SIGTERM) != 0 && errno == ESRCH) {
        // Already dead; still needs reaping so it does not linger as a zombie.
        int wait_status = 0;
        if (waitpid(target, &wait_status, 0) > 0) {
            ReapInto(wait_status);
        } else {
            state_ = ChildState::EXITED;
            pid_ = -1;
        }
        return;
    }

    // Poll rather than blocking outright, so a child that ignores SIGTERM or
    // wedges in cleanup cannot hang shutdown indefinitely.
    const int poll_interval_ms = 50;
    int waited_ms = 0;
    while (waited_ms < grace_period_ms) {
        int wait_status = 0;
        pid_t waited = waitpid(target, &wait_status, WNOHANG);
        if (waited == target) {
            ReapInto(wait_status);
            RK_LOGI("Stopped %s cleanly", name_.c_str());
            return;
        }
        if (waited < 0 && errno == ECHILD) {
            state_ = ChildState::EXITED;
            pid_ = -1;
            return;
        }
        SleepMilliseconds(poll_interval_ms);
        waited_ms += poll_interval_ms;
    }

    RK_LOGW("%s ignored SIGTERM after %d ms; sending SIGKILL", name_.c_str(),
            grace_period_ms);
    kill(target, SIGKILL);

    int wait_status = 0;
    if (waitpid(target, &wait_status, 0) > 0) {
        ReapInto(wait_status);
    } else {
        state_ = ChildState::EXITED;
        pid_ = -1;
    }
}

}  // namespace baby_monitor
