#include "base/child_process.h"

#include <csignal>
#include <cerrno>
#include <ctime>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include "base/plain_log.h"
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

    // Read before the fork, so the child can detect a parent that died inside
    // the fork/prctl window below.
    const pid_t supervisor_pid = getpid();

    pid_t forked = fork();
    if (forked < 0) {
        PLAIN_LOGE("fork() for %s failed: %d", name_.c_str(), errno);
        return false;
    }

    if (forked == 0) {
        // Child. Make the name visible in ps/top output, which is how an
        // operator tells these processes apart at a glance.
        prctl(PR_SET_NAME, name_.c_str());

        // Die with the supervisor. Without this, a supervisor that goes down
        // abnormally - SIGKILL, a crash, a debugger killing it, or a hardware
        // watchdog reset - leaves its workers running as orphans that still hold
        // the RTSP port, the camera and the audio capture device. The next launch
        // then fails to initialise all three, and it looks like broken hardware
        // rather than a leftover process.
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            PLAIN_LOGW("PR_SET_PDEATHSIG for %s failed: %d", name_.c_str(), errno);
        }

        // The parent can exit between the fork and the prctl above. PDEATHSIG
        // only fires for a death that happens after it is armed, so without this
        // re-check that window produces exactly the orphan it was meant to
        // prevent.
        if (getppid() != supervisor_pid) {
            _exit(0);
        }

        // The parent's signal dispositions and any blocked signals are
        // inherited; reset to defaults so the child's own handlers behave
        // predictably. SIGTERM and SIGINT matter as much as SIGCHLD: each
        // pipeline installs its handler at the top of Run(), so until then the
        // child still runs the supervisor's handler, which sets a flag only the
        // supervisor reads. That would swallow every SIGTERM arriving during
        // Initialise() - including the PDEATHSIG one armed above, and the one
        // Stop() sends, leaving SIGKILL as the only thing that works.
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        int status = entry_point_ ? entry_point_() : 0;

        // _exit, not exit: the child shares the parent's atexit handlers and
        // stdio buffers, and running them twice would double-flush.
        _exit(status);
    }

    pid_ = forked;
    state_ = ChildState::RUNNING;
    PLAIN_LOGI("Started %s as pid %d", name_.c_str(), static_cast<int>(pid_));
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
            PLAIN_LOGW("waitpid(%s) reports no such child", name_.c_str());
            state_ = ChildState::EXITED;
            pid_ = -1;
            return false;
        }
        PLAIN_LOGE("waitpid(%s) failed: %d", name_.c_str(), errno);
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
            PLAIN_LOGI("Stopped %s cleanly", name_.c_str());
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

    PLAIN_LOGW("%s ignored SIGTERM after %d ms; sending SIGKILL", name_.c_str(),
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
