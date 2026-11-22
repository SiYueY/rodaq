#pragma once

#include <memory>
#include <string_view>

#include <rclcpp/context.hpp>
#include <rcpputils/shared_library.hpp>

#include <rodaq/plugin/deadline.hpp>

namespace rodaq::ros2_acquisition {

class Context final : public rclcpp::Context {
public:
    // Only after shutdown and actual destruction of every Node/Executor/user.
    // Releases Context-owned subcontexts that can otherwise retain this Context
    // through a GuardCondition. Uses rclcpp's protected cleanup contract.
    Result<void> release_after_join();
};

// The SDK's dispatcher caches native implementation library handles in its
// mapped data. Pin their dispatcher owners for process lifetime, just as the
// Framework pins admitted Plugin DSOs. At most 256 canonical types per backend
// binary; no instance/config/token/payload or background work is retained.
Result<std::shared_ptr<rcpputils::SharedLibrary>> resident_typesupport(
    std::string_view type, plugin::Deadline deadline);

}  // namespace rodaq::ros2_acquisition
