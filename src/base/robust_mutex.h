// Process-shared robust mutex.
//
// Why a blocking mutex rather than the seqlock this project used before:
// RV1106 is a single-core Cortex-A7, so no two processes ever execute at the
// same instant. A spinning reader cannot make progress while the writer holds
// the data, it can only burn its own scheduling quantum until preempted.
// Blocking hands the core straight back to the writer, so the critical section
// completes sooner. On a single core, blocking strictly dominates spinning.
//
// PTHREAD_MUTEX_ROBUST additionally solves crash recovery: if the owner dies
// while holding the lock, the next locker receives EOWNERDEAD and can adopt
// the mutex via pthread_mutex_consistent(). That removes the permanent
// deadlock a plain process-shared mutex would leave behind.

#ifndef BABY_MONITOR_BASE_ROBUST_MUTEX_H
#define BABY_MONITOR_BASE_ROBUST_MUTEX_H

#include <pthread.h>

namespace baby_monitor {

// Lives inside shared memory, so it must stay a plain aggregate with no
// vtable and no pointers into any single process's address space.
struct SharedMutexStorage {
    pthread_mutex_t mutex;
};

// Prepares a mutex for cross-process use. Call exactly once, from whichever
// process creates the shared memory segment.
bool InitialiseSharedMutex(SharedMutexStorage* storage);

// Scoped lock that transparently recovers a mutex abandoned by a dead owner.
class SharedMutexGuard {
public:
    explicit SharedMutexGuard(SharedMutexStorage* storage);
    ~SharedMutexGuard();

    SharedMutexGuard(const SharedMutexGuard&) = delete;
    SharedMutexGuard& operator=(const SharedMutexGuard&) = delete;

    // False means the lock could not be taken and the guarded data must not be
    // touched. Always check before accessing shared state.
    bool locked() const { return locked_; }

    // True when this guard adopted a mutex whose previous owner died while
    // holding it. The protected data may be a partially applied update, so the
    // caller is responsible for deciding whether to repair or discard it.
    bool recovered_from_dead_owner() const { return recovered_from_dead_owner_; }

private:
    SharedMutexStorage* storage_;
    bool locked_;
    bool recovered_from_dead_owner_;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_BASE_ROBUST_MUTEX_H
