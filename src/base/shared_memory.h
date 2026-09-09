// RAII wrapper around a POSIX shared-memory segment holding exactly one T.
//
// Two distinct roles, deliberately separated into named factory functions
// rather than a bool flag, because getting them backwards is silent and awful
// to debug:
//
//   Create() - unlinks any stale segment, sizes a fresh one, zeroes it, and
//              runs the caller's initialiser. Exactly one process does this.
//   Attach() - opens a segment somebody else created. Fails if absent.
//
// A previous incarnation of this file leaked the descriptor and mapping on
// several error paths and threw from the constructor, which in a forked child
// meant an exception escaping into a half-initialised process.

#ifndef BABY_MONITOR_BASE_SHARED_MEMORY_H
#define BABY_MONITOR_BASE_SHARED_MEMORY_H

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "base/rk_platform.h"

namespace baby_monitor {

template <typename T>
class SharedMemory {
public:
    SharedMemory() = default;

    ~SharedMemory() { Reset(); }

    SharedMemory(SharedMemory&& other) noexcept
        : name_(std::move(other.name_)),
          mapping_(other.mapping_),
          owns_segment_(other.owns_segment_) {
        other.mapping_ = nullptr;
        other.owns_segment_ = false;
    }

    SharedMemory& operator=(SharedMemory&& other) noexcept {
        if (this != &other) {
            Reset();
            name_ = std::move(other.name_);
            mapping_ = other.mapping_;
            owns_segment_ = other.owns_segment_;
            other.mapping_ = nullptr;
            other.owns_segment_ = false;
        }
        return *this;
    }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // Creates the segment and takes responsibility for unlinking it. Any
    // leftover segment from a previous run is removed first, so a crash last
    // time cannot leave a wrongly-sized or half-initialised mapping behind.
    bool Create(const std::string& name) {
        Reset();
        shm_unlink(name.c_str());

        int descriptor = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (descriptor < 0) {
            RK_LOGE("shm_open(%s) for create failed: %s", name.c_str(), strerror(errno));
            return false;
        }

        bool mapped = MapDescriptor(descriptor, name, /*resize=*/true);
        close(descriptor);

        if (!mapped) {
            shm_unlink(name.c_str());
            return false;
        }

        memset(mapping_, 0, sizeof(T));
        name_ = name;
        owns_segment_ = true;
        return true;
    }

    // Attaches to a segment another process created. Does not unlink on
    // destruction: the creator owns the lifetime.
    bool Attach(const std::string& name) {
        Reset();

        int descriptor = shm_open(name.c_str(), O_RDWR, 0600);
        if (descriptor < 0) {
            RK_LOGE("shm_open(%s) for attach failed: %s", name.c_str(), strerror(errno));
            return false;
        }

        bool mapped = MapDescriptor(descriptor, name, /*resize=*/false);
        close(descriptor);

        if (!mapped) {
            return false;
        }

        name_ = name;
        owns_segment_ = false;
        return true;
    }

    bool valid() const { return mapping_ != nullptr; }

    T* get() { return mapping_; }
    const T* get() const { return mapping_; }

    T* operator->() { return mapping_; }
    const T* operator->() const { return mapping_; }

    // Removes a segment by name without needing an attached instance. Used at
    // startup to clear debris from an unclean shutdown.
    static void Unlink(const std::string& name) { shm_unlink(name.c_str()); }

private:
    bool MapDescriptor(int descriptor, const std::string& name, bool resize) {
        if (resize && ftruncate(descriptor, sizeof(T)) != 0) {
            RK_LOGE("ftruncate(%s, %zu) failed: %s", name.c_str(), sizeof(T),
                    strerror(errno));
            return false;
        }

        if (!resize) {
            // Guard against attaching to a segment left by an older build whose
            // record layout differs; the mapping would otherwise run past the end.
            struct stat segment_info;
            if (fstat(descriptor, &segment_info) != 0) {
                RK_LOGE("fstat(%s) failed: %s", name.c_str(), strerror(errno));
                return false;
            }
            if (static_cast<size_t>(segment_info.st_size) < sizeof(T)) {
                RK_LOGE("Segment %s is %lld bytes but %zu are required", name.c_str(),
                        static_cast<long long>(segment_info.st_size), sizeof(T));
                return false;
            }
        }

        void* address =
            mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (address == MAP_FAILED) {
            RK_LOGE("mmap(%s, %zu) failed: %s", name.c_str(), sizeof(T), strerror(errno));
            return false;
        }

        mapping_ = static_cast<T*>(address);
        return true;
    }

    void Reset() {
        if (mapping_ != nullptr) {
            munmap(mapping_, sizeof(T));
            mapping_ = nullptr;
        }
        if (owns_segment_ && !name_.empty()) {
            shm_unlink(name_.c_str());
        }
        owns_segment_ = false;
        name_.clear();
    }

    std::string name_;
    T* mapping_ = nullptr;
    bool owns_segment_ = false;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_BASE_SHARED_MEMORY_H
