#pragma once

#include <rodaq/export.hpp>

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
    RODAQ_API CloseFuture(CloseFuture&&) noexcept;
    RODAQ_API CloseFuture& operator=(CloseFuture&&) noexcept;

    RODAQ_API explicit operator bool() const noexcept;
    [[nodiscard]] RODAQ_API bool ready() const noexcept;
    [[nodiscard]] RODAQ_API CloseWaitStatus wait(Deadline deadline) const noexcept;
    RODAQ_API Result<void> result() const;

private:
    friend class PluginInstance;

    explicit CloseFuture(std::shared_ptr<InstanceState> state) noexcept;

    std::shared_ptr<InstanceState> state_;
};

}  // namespace rodaq::plugin
