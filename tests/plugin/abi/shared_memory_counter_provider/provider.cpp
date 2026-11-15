#include <counter.hpp>

#include <cstdint>
#include <limits>
#include <new>
#include <pthread.h>
#include <sys/mman.h>

// Qualification Backend: a real shared mapping with a process-shared mutex.
// It demonstrates SDK ownership, not crash-safe IPC or a persistent Counter.
class SharedMemoryCounter {
public:
    SharedMemoryCounter() = default;
    SharedMemoryCounter(const SharedMemoryCounter&) = delete;
    SharedMemoryCounter& operator=(const SharedMemoryCounter&) = delete;

    ~SharedMemoryCounter() noexcept {
        if (state_ != nullptr) {
            if (mutex_ready_) {
                pthread_mutex_destroy(&state_->mutex);
            }
            munmap(state_, sizeof(State));
        }
    }

    rodaq::Result<void> initialize() {
        void* mapping =
            mmap(nullptr, sizeof(State), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            return rodaq::Status(rodaq::StatusCode::ResourceExhausted, "mmap failed");
        }
        state_ = new (mapping) State{};
        pthread_mutexattr_t attributes;
        if (pthread_mutexattr_init(&attributes) != 0) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "mutex attributes failed");
        }
        const auto shared = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
        const auto initialized =
            shared == 0 ? pthread_mutex_init(&state_->mutex, &attributes) : shared;
        pthread_mutexattr_destroy(&attributes);
        mutex_ready_ = initialized == 0;
        return mutex_ready_ ? rodaq::Result<void>{}
                            : rodaq::Status(rodaq::StatusCode::Unavailable, "shared mutex failed");
    }

    rodaq::Result<std::int64_t> increment(std::int64_t delta) {
        if (pthread_mutex_lock(&state_->mutex) != 0) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "shared mutex lock failed");
        }
        const bool overflow =
            (delta > 0 && state_->value > std::numeric_limits<std::int64_t>::max() - delta) ||
            (delta < 0 && state_->value < std::numeric_limits<std::int64_t>::min() - delta);
        if (!overflow) {
            state_->value += delta;
        }
        const auto value = state_->value;
        pthread_mutex_unlock(&state_->mutex);
        return overflow ? rodaq::Result<std::int64_t>{rodaq::Status(
                              rodaq::StatusCode::InvalidArgument, "Counter overflow")}
                        : rodaq::Result<std::int64_t>{value};
    }

    rodaq::Result<std::int64_t> current() const {
        if (pthread_mutex_lock(&state_->mutex) != 0) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "shared mutex lock failed");
        }
        const auto value = state_->value;
        pthread_mutex_unlock(&state_->mutex);
        return value;
    }

private:
    struct State {
        pthread_mutex_t mutex{};
        std::int64_t value{0};
    };

    State* state_{nullptr};
    bool mutex_ready_{false};
};

class SharedMemoryCounterPlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto initialized = counter_.initialize();
        if (!initialized.ok()) {
            return initialized;
        }
        return setup.provide<CounterInterface>(counter_);
    }

    // In-flight Operations may finish after shutdown returns. Their shared
    // mapping is released only by destruction, after Framework Call Drain.
    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }

private:
    SharedMemoryCounter counter_;
};

RODAQ_DEFINE_PLUGIN(SharedMemoryCounterPlugin)
