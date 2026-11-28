#include "runtime_state.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

namespace rodaq::ros2_acquisition {
namespace {

template <typename Predicate>
bool wait_until(
    std::condition_variable& changed, std::unique_lock<std::mutex>& lock, plugin::Deadline deadline,
    Predicate ready) {
    if (deadline.is_infinite()) {
        changed.wait(lock, ready);
        return true;
    }
    auto remaining = deadline.remaining();
    return remaining.ok() && changed.wait_for(lock, remaining.value(), ready);
}

Status timeout() { return Status(StatusCode::DeadlineExceeded, "ROS 2 work remains owned"); }

}  // namespace

Backend::State::Permit::Permit(
    State* owner_in, Stream* stream_in, std::shared_ptr<const Snapshot> view)
: owner(owner_in), stream(stream_in), snapshot(std::move(view)) {}

Backend::State::Permit::Permit(Permit&& other) noexcept
: owner(std::exchange(other.owner, nullptr)),
  stream(other.stream),
  snapshot(std::move(other.snapshot)) {}

Backend::State::Permit::~Permit() {
    if (owner != nullptr) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        --owner->active;
        if (stream != nullptr) {
            --stream->active;
        }
        owner->changed.notify_all();
    }
}

Backend::State::Control::~Control() {
    std::lock_guard<std::mutex> lock(owner.mutex);
    owner.controlling = false;
    owner.changed.notify_all();
}

Result<void> Backend::State::enter_control(Phase expected, Phase next) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping || phase != expected || controlling) {
        return Status(StatusCode::Conflict, "ROS 2 lifecycle operation unavailable");
    }
    controlling = true;
    phase = next;
    return {};
}

Backend::State::Permit Backend::State::control_permit(plugin::Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!wait_until(changed, lock, deadline, [&] { return stopping || active < capacity; }) ||
        stopping) {
        return {};
    }
    ++active;
    return Permit(this, nullptr);
}

Backend::State::Permit Backend::State::data_permit(Stream& stream) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping || phase != Phase::Running) {
        return {};
    }
    if (!stream.gate || !stream.snapshot) {
        if (stream.lost != std::numeric_limits<std::uint64_t>::max()) {
            ++stream.lost;
        }
        return {};
    }
    if (active == capacity) {
        if (stream.overflow != std::numeric_limits<std::uint64_t>::max()) {
            ++stream.overflow;
        }
        return {};
    }
    ++active;
    ++stream.active;
    return Permit(this, &stream, stream.snapshot);
}

void Backend::State::report_event(
    rodaq_acquisition_event_kind_t kind, rodaq_stream_id_t stream, StatusCode status,
    std::uint64_t count) noexcept {
    acquisition::AcquisitionEvent report{};
    report.header.struct_size = sizeof(report);
    report.kind = kind;
    report.status = static_cast<rodaq_status_code_t>(status);
    report.stream_id = stream;
    report.has_stream_id = stream != 0 ? 1 : 0;
    report.count = count;
    callbacks.report_event(report);
}

Backend::Backend() : state_(std::make_unique<State>()) {}

Backend::~Backend() noexcept { (void)quiesce(plugin::Deadline::infinite()); }

Result<void> Backend::configure(std::string_view input) {
    auto parsed = parse_config(input);
    if (!parsed.ok()) {
        return parsed.status();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->phase != State::Phase::New || state_->stopping) {
        return Status(StatusCode::Conflict, "ROS 2 already configured or stopped");
    }
    state_->config.emplace(std::move(parsed.value()));
    state_->phase = State::Phase::Configured;
    return {};
}

Result<void> Backend::prepare(
    const acquisition::PrepareConfig& limits, acquisition::Callbacks callbacks,
    plugin::Deadline deadline) {
    auto& state = *state_;
    auto entered = state.enter_control(State::Phase::Configured, State::Phase::Preparing);
    if (!entered.ok()) {
        return entered;
    }
    State::Control control{state};
    if (!callbacks || limits.stream_count > 256 || limits.max_callback_concurrency > 256 ||
        !acquisition::abi::valid_prepare(&limits) ||
        limits.stream_count != state.config->streams.size()) {
        return Status(StatusCode::InvalidArgument, "Invalid ROS 2 prepare bounds");
    }
    for (std::uint32_t i = 0; i < limits.stream_count; ++i) {
        for (std::uint32_t j = 0; j < i; ++j) {
            const auto& left = limits.streams[i];
            const auto& right = limits.streams[j];
            if (left.stream_id == right.stream_id ||
                std::string_view(left.stream_key.data, left.stream_key.size) ==
                    std::string_view(right.stream_key.data, right.stream_key.size)) {
                return Status(StatusCode::InvalidArgument, "Duplicate ROS 2 stream binding");
            }
        }
    }
    auto remaining = deadline.remaining();
    if (!remaining.ok() || remaining.value().count() == 0) {
        return timeout();
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.callbacks = callbacks;
        state.capacity = limits.max_callback_concurrency;
    }
    try {
        for (const auto& configured : state.config->streams) {
            const auto* end = limits.streams + limits.stream_count;
            const auto* binding = std::find_if(limits.streams, end, [&](const auto& item) {
                return std::string_view(item.stream_key.data, item.stream_key.size) ==
                       configured.key;
            });
            if (binding == end) {
                return Status(StatusCode::InvalidArgument, "ROS 2 stream binding mismatch");
            }
            auto stream = std::make_unique<State::Stream>();
            stream->config = configured;
            stream->id = binding->stream_id;
            state.streams.push_back(std::move(stream));
        }
        state.context = std::make_shared<Context>();
        rclcpp::InitOptions init;
        init.set_domain_id(state.config->domain_id);
        init.auto_initialize_logging(false);
        state.context->init(0, nullptr, init);
        static std::atomic<std::uint64_t> next_node{1};
        rclcpp::NodeOptions options;
        options.context(state.context)
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false);
        state.node = std::make_shared<rclcpp::Node>(
            "rodaq_" + std::to_string(next_node.fetch_add(1)), options);
        rclcpp::ExecutorOptions executor_options;
        executor_options.context = state.context;
        state.executor = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
            executor_options, state.config->executor_threads);
        state.executor->add_node(state.node);

        auto permit = state.control_permit(deadline);
        if (!permit) {
            return timeout();
        }
        state.report_event(RODAQ_ACQUISITION_EVENT_TRANSPORT_READY, 0);
        for (auto& stream : state.streams) {
            stream->group =
                state.node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
            auto facts = state.inspect(*stream);
            // Discovery is asynchronous. With no configured type, wait within
            // prepare budget; an explicit type allows 0-publisher/0-Hz prepare.
            while (!facts.ok() && facts.status().code() == StatusCode::Unavailable &&
                   !stream->config.type) {
                auto budget = deadline.remaining();
                if (!budget.ok() || budget.value().count() == 0) {
                    return facts.status();
                }
                {
                    std::unique_lock<std::mutex> lock(state.mutex);
                    if (state.changed.wait_for(
                            lock, std::min(budget.value(), state.config->graph_poll_interval),
                            [&] { return state.stopping; })) {
                        return timeout();
                    }
                }
                facts = state.inspect(*stream);
            }
            if (!facts.ok()) {
                return facts.status();
            }
            auto bound = state.bind_stream(*stream, std::move(facts.value()), deadline);
            if (!bound.ok()) {
                return bound;
            }
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.stopping) {
            return timeout();
        }
        state.phase = State::Phase::Prepared;
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "ROS 2 preparation allocation failed");
    } catch (const std::exception&) {
        return Status(StatusCode::Unavailable, "ROS 2 preparation failed");
    } catch (...) {
        return Status(StatusCode::Internal, "Unknown ROS 2 preparation exception");
    }
}

Result<void> Backend::start(plugin::Deadline deadline) {
    auto& state = *state_;
    auto remaining = deadline.remaining();
    if (!remaining.ok() || remaining.value().count() == 0) {
        return timeout();
    }
    auto entered = state.enter_control(State::Phase::Prepared, State::Phase::Running);
    if (!entered.ok()) {
        return entered;
    }
    State::Control control{state};
    try {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            for (auto& stream : state.streams) {
                stream->gate = true;
            }
            state.spin_done = false;
        }
        state.spin_thread = std::thread([&state] { state.spin(); });
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.graph_done = false;
        }
        state.graph_thread = std::thread([&state] { state.observe(); });
        return {};
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stopping = true;
            state.spin_done = !state.spin_thread.joinable();
            state.graph_done = !state.graph_thread.joinable();
        }
        state.cancel();
        return Status(StatusCode::ResourceExhausted, "ROS 2 execution thread creation failed");
    }
}

void Backend::State::cancel() noexcept {
    try {
        if (executor) {
            executor->cancel();
        }
        if (context && context->is_valid()) {
            context->shutdown("RoDAQ instance stopped");
        }
    } catch (...) {
    }
}

Result<void> Backend::quiesce(plugin::Deadline deadline) {
    auto& state = *state_;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stopping = true;
        state.changed.notify_all();
    }
    std::unique_lock<std::timed_mutex> cleanup(state.cleanup, std::defer_lock);
    if (deadline.is_infinite()) {
        cleanup.lock();
    } else {
        auto remaining = deadline.remaining();
        if (!remaining.ok() || !cleanup.try_lock_for(remaining.value())) {
            return timeout();
        }
    }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!wait_until(state.changed, lock, deadline, [&] { return !state.controlling; })) {
            return timeout();
        }
    }
    state.cancel();
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!wait_until(state.changed, lock, deadline, [&] {
                return state.active == 0 && state.spin_done && state.graph_done;
            })) {
            return timeout();
        }
    }
    if (state.spin_thread.joinable()) {
        state.spin_thread.join();
    }
    if (state.graph_thread.joinable()) {
        state.graph_thread.join();
    }
    if (state.callbacks && state.capacity != 0) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.active;
        }
        State::Permit permit(&state, nullptr);
        for (auto& stream : state.streams) {
            state.flush_events(*stream);
        }
    }
    state.streams.clear();
    state.executor.reset();
    state.node.reset();
    if (state.context) {
        auto released = state.context->release_after_join();
        if (!released.ok()) {
            return released;
        }
    }
    state.context.reset();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks = {};
    state.phase = State::Phase::Stopped;
    return {};
}

void Backend::State::spin() noexcept {
    try {
        executor->spin();
    } catch (...) {
        fail(StatusCode::Internal);
    }
    std::lock_guard<std::mutex> lock(mutex);
    spin_done = true;
    changed.notify_all();
}

void Backend::State::fail(StatusCode status) noexcept {
    try {
        auto permit =
            control_permit(plugin::Deadline::after(std::chrono::milliseconds{100}).value());
        if (permit) {
            report_event(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, 0, status);
        }
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
    changed.notify_all();
}

}  // namespace rodaq::ros2_acquisition
