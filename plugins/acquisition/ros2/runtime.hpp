#pragma once

#include <memory>
#include <string_view>

#include <rodaq/acquisition/service.hpp>
#include <rodaq/acquisition/types.hpp>

namespace rodaq::ros2_acquisition {

// Owns a private ROS execution domain. Quiesce timeout retains every participant;
// destruction waits for actual native return and never detaches a thread.
class Backend {
public:
    Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    ~Backend() noexcept;

    Result<void> configure(std::string_view input, acquisition::ServiceReference service);
    Result<void> prepare(const acquisition::PrepareConfig& config, plugin::Deadline deadline);
    Result<void> start(plugin::Deadline deadline);
    Result<void> quiesce(plugin::Deadline deadline);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::ros2_acquisition
