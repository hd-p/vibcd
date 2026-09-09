#include "base/robust_mutex.h"

#include <cerrno>

#include "base/rk_platform.h"

namespace baby_monitor {

bool InitialiseSharedMutex(SharedMutexStorage* storage) {
    if (storage == nullptr) {
        return false;
    }

    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) {
        RK_LOGE("pthread_mutexattr_init failed");
        return false;
    }

    bool configured = true;

    // Without PTHREAD_PROCESS_SHARED the mutex only synchronises threads of the
    // creating process, and every other process would sail straight through it.
    if (pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) != 0) {
        RK_LOGE("pthread_mutexattr_setpshared failed");
        configured = false;
    }

    if (configured &&
        pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) != 0) {
        RK_LOGE("pthread_mutexattr_setrobust failed");
        configured = false;
    }

    if (configured && pthread_mutex_init(&storage->mutex, &attributes) != 0) {
        RK_LOGE("pthread_mutex_init failed");
        configured = false;
    }

    pthread_mutexattr_destroy(&attributes);
    return configured;
}

SharedMutexGuard::SharedMutexGuard(SharedMutexStorage* storage)
    : storage_(storage), locked_(false), recovered_from_dead_owner_(false) {
    if (storage_ == nullptr) {
        return;
    }

    int lock_result = pthread_mutex_lock(&storage_->mutex);

    if (lock_result == EOWNERDEAD) {
        // The previous owner died mid-update. Mark the mutex consistent so it
        // stays usable; the caller inspects recovered_from_dead_owner() to
        // decide what to do about the possibly half-written payload.
        if (pthread_mutex_consistent(&storage_->mutex) == 0) {
            locked_ = true;
            recovered_from_dead_owner_ = true;
            RK_LOGW("Recovered shared mutex abandoned by a dead owner");
        } else {
            RK_LOGE("pthread_mutex_consistent failed; mutex is unusable");
            pthread_mutex_unlock(&storage_->mutex);
        }
        return;
    }

    if (lock_result == ENOTRECOVERABLE) {
        // A previous owner died and nobody managed to restore consistency.
        // The mutex is permanently dead by design; report it rather than
        // pretending the data is safe to touch.
        RK_LOGE("Shared mutex is not recoverable");
        return;
    }

    if (lock_result != 0) {
        RK_LOGE("pthread_mutex_lock failed: %d", lock_result);
        return;
    }

    locked_ = true;
}

SharedMutexGuard::~SharedMutexGuard() {
    if (locked_ && storage_ != nullptr) {
        pthread_mutex_unlock(&storage_->mutex);
    }
}

}  // namespace baby_monitor
