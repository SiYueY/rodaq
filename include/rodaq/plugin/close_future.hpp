#pragma once

#include <memory>

#include <rodaq/plugin/deadline.hpp>

namespace rodaq::plugin {

struct InstanceState;
class PluginInstance;

enum class CloseWaitStatus { Completed, TimedOut };

/** Shared terminal observer. TimedOut does not cancel or settle native close work. */
class CloseFuture {
public:
    CloseFuture() = delete;
    CloseFuture(const CloseFuture&) noexcept = default;
    CloseFuture& operator=(const CloseFuture&) noexcept = default;
    CloseFuture(CloseFuture&&) noexcept;
    CloseFuture& operator=(CloseFuture&&) noexcept;

    explicit operator bool() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] CloseWaitStatus wait(Deadline deadline) const noexcept;
    Result<void> result() const;

private:
    friend class PluginInstance;

    explicit CloseFuture(std::shared_ptr<InstanceState> state) noexcept;

    std::shared_ptr<InstanceState> state_;
};

}  // namespace rodaq::plugin
