#pragma once

#include "runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "config.hpp"
#include "native_resources.hpp"
#include "ros2/schema.hpp"

namespace rodaq::ros2_acquisition {

struct Backend::State {
    enum class Phase { New, Configured, Preparing, Prepared, Running, Stopped };

    struct Facts {
        std::string type;
        std::vector<rodaq_ros2_qos_profile_t> offers;
        std::size_t compatible_publishers{0};
    };

    // Immutable bind-once Descriptor & subscription snapshot created during prepare().
    struct Snapshot {
        ros2::Schema schema;
        Facts facts;
        ros2::RequestedQos installed;
        rodaq_ros2_qos_profile_t actual_request{};
        rodaq_descriptor_id_t descriptor_id{0};
    };

    struct Stream {
        StreamConfig config;
        rodaq_stream_id_t id{0};
        rclcpp::CallbackGroup::SharedPtr group;
        rclcpp::GenericSubscription::SharedPtr subscription;
        std::shared_ptr<const Snapshot> snapshot;
        bool gate{false};
        bool ready_reported{false};
        std::uint32_t active{0};
        std::uint64_t overflow{0};
        std::uint64_t lost{0};
    };

    // Each successful permit covers one actual call chain into Core. Data never
    // waits for capacity. Control waits only within its supplied budget.
    struct Permit {
        State* owner{nullptr};
        Stream* stream{nullptr};
        std::shared_ptr<const Snapshot> snapshot;
        Permit() = default;
        Permit(State* owner_in, Stream* stream_in, std::shared_ptr<const Snapshot> view = {});
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept;
        ~Permit();
        explicit operator bool() const noexcept { return owner != nullptr; }
    };

    struct Control {
        State& owner;
        ~Control();
    };

    Result<void> enter_control(Phase expected, Phase next);
    Permit control_permit(plugin::Deadline deadline);
    Permit data_permit(Stream& stream);
    void report_event(
        rodaq_acquisition_event_kind_t kind, rodaq_stream_id_t stream,
        StatusCode status = StatusCode::Ok, std::uint64_t count = 0) noexcept;
    void flush_events(Stream& stream) noexcept;
    Result<Facts> inspect(const Stream& stream);
    Result<void> bind_stream(Stream& stream, Facts facts, plugin::Deadline deadline);
    rclcpp::GenericSubscription::SharedPtr subscribe(Stream& stream, const Snapshot& snapshot);
    void dispatch(
        Stream& stream, const std::shared_ptr<rclcpp::SerializedMessage>& message,
        const rmw_message_info_t* metadata) noexcept;
    void spin() noexcept;
    void observe() noexcept;
    void fail(StatusCode status) noexcept;
    void cancel() noexcept;

    std::optional<Config> config;
    acquisition::Callbacks callbacks;
    std::vector<std::unique_ptr<Stream>> streams;
    std::shared_ptr<Context> context;
    rclcpp::Node::SharedPtr node;
    std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor;
    std::thread spin_thread;
    std::thread graph_thread;

    // Never held across Core, ROS, filesystem or thread join operations.
    std::mutex mutex;
    std::condition_variable changed;
    std::timed_mutex cleanup;
    Phase phase{Phase::New};
    bool stopping{false};
    bool controlling{false};
    bool spin_done{true};
    bool graph_done{true};
    std::uint32_t capacity{0};
    std::uint32_t active{0};
};

}  // namespace rodaq::ros2_acquisition
