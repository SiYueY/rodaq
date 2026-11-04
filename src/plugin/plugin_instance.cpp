#include "plugin/plugin_instance.hpp"

#include <rodaq/plugin/abi_support.hpp>
#include "plugin/plugin_error.hpp"
#include "plugin/plugin_library.hpp"

#include <atomic>
#include <memory>
#include <string_view>
#include <utility>

namespace rodaq::plugin {

class PluginInstanceState {
public:
    using BindFunction = PluginInstanceStateAccess::BindFunction;
    using ConfigureFunction = PluginInstanceStateAccess::ConfigureFunction;
    using QueryInterfaceFunction = PluginInstanceStateAccess::QueryInterfaceFunction;
    using StartFunction = PluginInstanceStateAccess::StartFunction;
    using StopFunction = PluginInstanceStateAccess::StopFunction;
    using OpenFunction = PluginInstanceStateAccess::OpenFunction;
    using SubmitFunction = PluginInstanceStateAccess::SubmitFunction;
    using FlushFunction = PluginInstanceStateAccess::FlushFunction;
    using CloseFunction = PluginInstanceStateAccess::CloseFunction;

    PluginInstanceState(
        PluginLibraryLease library_lease, void* instance, void (*destroy)(void*) noexcept,
        BindFunction bind_function, ConfigureFunction configure_function,
        QueryInterfaceFunction query_interface_function, StartFunction start_function,
        StopFunction stop_function, OpenFunction open_function, SubmitFunction submit_function,
        FlushFunction flush_function, CloseFunction close_function)
    : library_lease_(std::move(library_lease)),
      instance_(instance),
      destroy_(destroy),
      bind_(bind_function),
      configure_(configure_function),
      query_interface_(query_interface_function),
      start_(start_function),
      stop_(stop_function),
      open_(open_function),
      submit_(submit_function),
      flush_(flush_function),
      close_(close_function) {}

    ~PluginInstanceState() {
        if (instance_ == nullptr || destroy_ == nullptr) {
            return;
        }
        if (quarantined_.load(std::memory_order_acquire)) {
            library_lease_.quarantine();
        } else {
            destroy_(instance_);
        }
        instance_ = nullptr;
    }

    void quarantine() noexcept {
        if (!quarantined_.exchange(true, std::memory_order_acq_rel)) {
            library_lease_.quarantine();
        }
    }

    Result<void> bind(const rodaq_service_registry& services) {
        if (stage_ != LifecycleStage::Created) {
            return invalid_lifecycle("Plugin bind requires Created state");
        }
        if (instance_ == nullptr || bind_ == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidState, "plugin", 0,
                "Plugin instance cannot bind in its current state"));
        }
        if (services.context == nullptr || services.vtable == nullptr) {
            return nonstd::make_unexpected(
                invalid_argument_error("Plugin bind requires a valid Service Registry"));
        }

        rodaq_error status_error{};
        const rodaq_status status = bind_(instance_, &services, &status_error);
        Result<void> converted = result_from_status(status, &status_error, "plugin");
        if (converted.has_value()) {
            stage_ = LifecycleStage::Bound;
        }
        return converted;
    }

    Result<void> configure(rodaq_config_view config) {
        if (stage_ != LifecycleStage::Bound) {
            return invalid_lifecycle("Plugin configure requires Bound state");
        }
        if (instance_ == nullptr || configure_ == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidState, "plugin", 0,
                "Plugin instance cannot configure in its current state"));
        }
        if (config.context == nullptr || config.vtable == nullptr) {
            return nonstd::make_unexpected(
                invalid_argument_error("Plugin configure requires a valid ConfigView"));
        }

        rodaq_error status_error{};
        const rodaq_status status = configure_(instance_, config, &status_error);
        Result<void> converted = result_from_status(status, &status_error, "plugin");
        if (converted.has_value()) {
            stage_ = LifecycleStage::Configured;
        }
        return converted;
    }

    Result<rodaq_interface> query_interface(std::string_view interface_id) {
        if (instance_ == nullptr || query_interface_ == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidState, "plugin", 0,
                "Plugin instance cannot query interfaces in its current state"));
        }
        if (interface_id.empty() || !rodaq::is_valid_utf8_without_nul(interface_id)) {
            return nonstd::make_unexpected(
                invalid_argument_error("Plugin interface ID must be non-empty UTF-8 without NUL"));
        }

        rodaq_interface interface {};
        rodaq_error status_error{};
        const rodaq_status status = query_interface_(
            instance_, rodaq_string_view{interface_id.data(), interface_id.size()}, &interface,
            &status_error);
        Result<void> converted = result_from_status(status, &status_error, "plugin");
        if (!converted.has_value()) {
            return nonstd::make_unexpected(std::move(converted.error()));
        }
        if (interface.context == nullptr || interface.vtable == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidArgument, "plugin", 0,
                "Plugin query_interface returned OK with an incomplete interface"));
        }
        return interface;
    }

    void set_required_control_interface(rodaq_interface interface) noexcept {
        required_control_interface_ = interface;
    }

    Result<rodaq_interface> required_control_interface() const {
        if (required_control_interface_.context == nullptr ||
            required_control_interface_.vtable == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidState, "plugin", 0,
                "Required Plugin control interface is not available"));
        }
        return required_control_interface_;
    }

    Result<void> start() {
        if (stage_ != LifecycleStage::Configured || start_ == nullptr) {
            return invalid_lifecycle("Plugin start requires Configured state");
        }
        stage_ = LifecycleStage::StartAttempted;
        rodaq_error error{};
        Result<void> result = result_from_status(start_(instance_, &error), &error, "plugin");
        if (result.has_value()) {
            stage_ = LifecycleStage::Started;
        }
        return result;
    }

    Result<void> stop() {
        if ((stage_ != LifecycleStage::Started && stage_ != LifecycleStage::StartAttempted) ||
            stop_ == nullptr) {
            return invalid_lifecycle("Plugin stop requires Started or StartAttempted state");
        }
        rodaq_error error{};
        Result<void> result = result_from_status(stop_(instance_, &error), &error, "plugin");
        if (result.has_value()) {
            stage_ = LifecycleStage::Stopped;
        }
        return result;
    }

    Result<void> open(const rodaq_storage_open_request& request) {
        if (stage_ != LifecycleStage::Configured || open_ == nullptr) {
            return invalid_lifecycle("Storage open requires Configured state");
        }
        rodaq_error error{};
        Result<void> result =
            result_from_status(open_(instance_, &request, &error), &error, "plugin");
        if (result.has_value()) {
            stage_ = LifecycleStage::Opened;
        }
        return result;
    }

    Result<rodaq_storage_submit_result> submit(const rodaq_storage_submission& submission) {
        if (stage_ != LifecycleStage::Opened || submit_ == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::InvalidState, "plugin", 0, "Storage submit requires Opened state"));
        }
        rodaq_storage_submit_result output{};
        output.struct_size = sizeof(output);
        rodaq_error error{};
        const rodaq_status status = submit_(instance_, &submission, &output, &error);
        if (status != RODAQ_STATUS_OK) {
            return nonstd::make_unexpected(result_from_status(status, &error, "plugin").error());
        }
        return output;
    }

    Result<void> flush() {
        if (stage_ != LifecycleStage::Opened || flush_ == nullptr) {
            return invalid_lifecycle("Storage flush requires Opened state");
        }
        rodaq_error error{};
        return result_from_status(flush_(instance_, &error), &error, "plugin");
    }

    Result<void> close(const rodaq_storage_close_request& request) {
        if (stage_ != LifecycleStage::Opened || close_ == nullptr) {
            return invalid_lifecycle("Storage close requires Opened state");
        }
        rodaq_error error{};
        Result<void> result =
            result_from_status(close_(instance_, &request, &error), &error, "plugin");
        if (result.has_value()) {
            stage_ = LifecycleStage::Closed;
        }
        return result;
    }

private:
    enum class LifecycleStage {
        Created,
        Bound,
        Configured,
        StartAttempted,
        Started,
        Stopped,
        Opened,
        Closed,
    };

    static Result<void> invalid_lifecycle(const char* message) {
        return nonstd::make_unexpected(Error(ErrorCode::InvalidState, "plugin", 0, message));
    }

    PluginLibraryLease library_lease_;
    void* instance_;
    void (*destroy_)(void*) noexcept;
    BindFunction bind_;
    ConfigureFunction configure_;
    QueryInterfaceFunction query_interface_;
    StartFunction start_;
    StopFunction stop_;
    OpenFunction open_;
    SubmitFunction submit_;
    FlushFunction flush_;
    CloseFunction close_;
    LifecycleStage stage_ = LifecycleStage::Created;
    rodaq_interface required_control_interface_{};
    std::atomic<bool> quarantined_{false};

    friend class PluginInstanceStateAccess;
};

std::shared_ptr<PluginInstanceState> PluginInstanceStateAccess::create(
    const PluginLibraryLease& library_lease, void* instance, void (*destroy)(void*) noexcept,
    BindFunction bind_function, ConfigureFunction configure_function,
    QueryInterfaceFunction query_interface_function, StartFunction start_function,
    StopFunction stop_function, OpenFunction open_function, SubmitFunction submit_function,
    FlushFunction flush_function, CloseFunction close_function) {
    return std::make_shared<PluginInstanceState>(
        library_lease, instance, destroy, bind_function, configure_function,
        query_interface_function, start_function, stop_function, open_function, submit_function,
        flush_function, close_function);
}

Result<void> PluginInstanceStateAccess::bind(
    const std::shared_ptr<PluginInstanceState>& state, const rodaq_service_registry& services) {
    if (state == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "plugin", 0, "Plugin instance is empty"));
    }
    return state->bind(services);
}

Result<void> PluginInstanceStateAccess::configure(
    const std::shared_ptr<PluginInstanceState>& state, rodaq_config_view config) {
    if (state == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "plugin", 0, "Plugin instance is empty"));
    }
    return state->configure(config);
}

Result<rodaq_interface> PluginInstanceStateAccess::query_interface(
    const std::shared_ptr<PluginInstanceState>& state, std::string_view interface_id) {
    if (state == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "plugin", 0, "Plugin instance is empty"));
    }
    return state->query_interface(interface_id);
}

Result<void> PluginInstanceStateAccess::start(const std::shared_ptr<PluginInstanceState>& state) {
    if (state == nullptr) {
        return PluginInstanceState::invalid_lifecycle("Plugin instance is empty");
    }
    return state->start();
}

Result<void> PluginInstanceStateAccess::stop(const std::shared_ptr<PluginInstanceState>& state) {
    if (state == nullptr) {
        return PluginInstanceState::invalid_lifecycle("Plugin instance is empty");
    }
    return state->stop();
}

Result<void> PluginInstanceStateAccess::open(
    const std::shared_ptr<PluginInstanceState>& state, const rodaq_storage_open_request& request) {
    if (state == nullptr) {
        return PluginInstanceState::invalid_lifecycle("Plugin instance is empty");
    }
    return state->open(request);
}

Result<rodaq_storage_submit_result> PluginInstanceStateAccess::submit(
    const std::shared_ptr<PluginInstanceState>& state, const rodaq_storage_submission& submission) {
    if (state == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "plugin", 0, "Plugin instance is empty"));
    }
    return state->submit(submission);
}

Result<void> PluginInstanceStateAccess::flush(const std::shared_ptr<PluginInstanceState>& state) {
    if (state == nullptr) {
        return PluginInstanceState::invalid_lifecycle("Plugin instance is empty");
    }
    return state->flush();
}

Result<void> PluginInstanceStateAccess::close(
    const std::shared_ptr<PluginInstanceState>& state, const rodaq_storage_close_request& request) {
    if (state == nullptr) {
        return PluginInstanceState::invalid_lifecycle("Plugin instance is empty");
    }
    return state->close(request);
}

void PluginInstanceStateAccess::set_required_control_interface(
    const std::shared_ptr<PluginInstanceState>& state, rodaq_interface interface) noexcept {
    if (state != nullptr) {
        state->set_required_control_interface(interface);
    }
}

Result<rodaq_interface> PluginInstanceStateAccess::required_control_interface(
    const std::shared_ptr<PluginInstanceState>& state) {
    if (state == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "plugin", 0, "Plugin instance is empty"));
    }
    return state->required_control_interface();
}

PluginInstanceLease::PluginInstanceLease(std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

bool PluginInstanceLease::valid() const noexcept { return state_ != nullptr; }

void PluginInstanceLease::quarantine() noexcept {
    if (state_ != nullptr) {
        state_->quarantine();
    }
}

}  // namespace rodaq::plugin
