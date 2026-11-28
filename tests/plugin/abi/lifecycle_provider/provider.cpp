#include "counter.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

// Deliberate blocking/failure modes are confined to this qualification DSO.
static void wait_at_gate(const char* gate, const char* entered) noexcept {
    if (gate == nullptr || entered == nullptr) {
        std::_Exit(53);
    }
    const auto marker = open(entered, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (marker >= 0) {
        close(marker);
    }
    const auto file = open(gate, O_RDONLY | O_CLOEXEC);
    if (file >= 0) {
        char byte = 0;
        while (read(file, &byte, 1) < 0 && errno == EINTR) {
        }
        close(file);
    }
}

class Counter {
public:
    rodaq::Result<std::int64_t> increment(std::int64_t delta) {
        if (wait_on_increment && gate != nullptr) {
            wait_at_gate(gate, entered);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if ((delta > 0 && value_ > std::numeric_limits<std::int64_t>::max() - delta) ||
            (delta < 0 && value_ < std::numeric_limits<std::int64_t>::min() - delta)) {
            return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Counter overflow");
        }
        value_ += delta;
        return value_;
    }

    rodaq::Result<std::int64_t> current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    bool wait_on_increment{false};
    const char* gate{nullptr};
    const char* entered{nullptr};

private:
    mutable std::mutex mutex_;
    std::int64_t value_{0};
};

class LifecyclePlugin final : public rodaq::plugin::PluginBase {
public:
    ~LifecyclePlugin() noexcept override {
        record("destroy\n");
        if (mode_ == "destroy_wait") {
            wait_gate();
        }
    }

    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        try {
            const auto config =
                nlohmann::json::parse(setup.config().empty() ? "{}" : setup.config());
            if (!config.is_object()) {
                return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Expected fixture object");
            }
            mode_ = config.value("mode", "normal");
            gate_ = config.value("gate", "");
            entered_ = gate_ + ".entered";
            events_ = config.value("events", "");
        } catch (const nlohmann::json::exception&) {
            return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Invalid fixture JSON");
        }
        record("configure\n");
        if (mode_ == "configure_wait") {
            wait_gate();
        }
        if (mode_ == "configure_fail" || mode_ == "configure_fail_wait") {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "Primary configure failure");
        }
        if (mode_ == "operation_wait") {
            counter_.wait_on_increment = true;
            counter_.gate = gate_.c_str();
            counter_.entered = entered_.c_str();
        }
        return setup.provide<CounterInterface>(counter_);
    }

    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        record("shutdown\n");
        if (mode_ == "shutdown_wait" || mode_ == "configure_fail_wait") {
            wait_gate();
        }
        if (mode_ == "shutdown_fail") {
            return rodaq::Status(rodaq::StatusCode::DeadlineExceeded, "Native terminal failure");
        }
        return {};
    }

    void before_query() const noexcept {
        if (mode_ == "query_wait") {
            wait_gate();
        }
    }

    const rodaq_counter_vtable_t* counter_table() const noexcept { return &counter_table_; }

    std::string_view mode() const noexcept { return mode_; }

private:
    void record(std::string_view text) const noexcept {
        if (events_.empty()) {
            return;
        }
        const auto file = open(events_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (file >= 0) {
            const auto count = write(file, text.data(), text.size());
            (void)count;
            close(file);
        }
    }

    void wait_gate() const noexcept { wait_at_gate(gate_.c_str(), entered_.c_str()); }

    Counter counter_;
    const rodaq_counter_vtable_t counter_table_{*CounterProvider<Counter>::vtable()};
    std::string mode_;
    std::string gate_;
    std::string entered_;
    std::string events_;
};

using NativeInstance = rodaq::plugin::authoring::Instance<LifecyclePlugin>;

static rodaq_status_code_t fixture_query(
    rodaq_plugin_context_t* context, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept {
    if (context == nullptr) {
        return NativeInstance::query(context, query, reference, error);
    }
    auto& plugin = reinterpret_cast<NativeInstance*>(context)->plugin;
    plugin.before_query();
    const auto code = NativeInstance::query(context, query, reference, error);
    if (plugin.mode() == "unknown_query") {
        return 99;
    }
    if (code == RODAQ_STATUS_OK) {
        reference->vtable =
            reinterpret_cast<const rodaq_interface_vtable_t*>(plugin.counter_table());
        reference->vtable_size = sizeof(rodaq_counter_vtable_t);
    }
    return code;
}

static rodaq_status_code_t fixture_configure(
    rodaq_plugin_context_t* context, const rodaq_application_callbacks_t* application,
    const rodaq_config_t* config, rodaq_error_t* error) noexcept {
    const auto code = NativeInstance::configure(context, application, config, error);
    if (context == nullptr) {
        return code;
    }
    return reinterpret_cast<NativeInstance*>(context)->plugin.mode() == "unknown_configure" ? 99
                                                                                            : code;
}

static rodaq_status_code_t fixture_shutdown(
    rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) noexcept {
    const auto code = NativeInstance::shutdown(context, deadline, error);
    if (context == nullptr) {
        return code;
    }
    return reinterpret_cast<NativeInstance*>(context)->plugin.mode() == "unknown_shutdown" ? 99
                                                                                           : code;
}

static rodaq_status_code_t poisoned_shutdown(
    rodaq_plugin_context_t*, rodaq_deadline_t, rodaq_error_t*) {
    std::_Exit(51);
}

static void poisoned_destroy(rodaq_plugin_context_t*) { std::_Exit(52); }

static const rodaq_plugin_instance_vtable_t kFixtureVtable{
    {sizeof(kFixtureVtable)},
    fixture_configure,
    fixture_query,
    fixture_shutdown,
    NativeInstance::destroy};
static const rodaq_plugin_instance_vtable_t kPoisonedVtable{
    {sizeof(kPoisonedVtable)},
    fixture_configure,
    fixture_query,
    poisoned_shutdown,
    poisoned_destroy};
static const rodaq_plugin_instance_vtable_t kShortVtable{
    {sizeof(rodaq_struct_header_t)},
    fixture_configure,
    fixture_query,
    poisoned_shutdown,
    poisoned_destroy};

extern "C" rodaq_status_code_t rodaq_plugin_create_instance(
    rodaq_plugin_instance_t* output, rodaq_error_t* error) noexcept {
    const auto* mode = std::getenv("RODAQ_FIXTURE_CREATE_MODE");
    if (mode != nullptr && std::strcmp(mode, "wait") == 0) {
        wait_at_gate(
            std::getenv("RODAQ_FIXTURE_CREATE_GATE"), std::getenv("RODAQ_FIXTURE_CREATE_ENTERED"));
    }
    if (mode != nullptr &&
        (std::strcmp(mode, "normal_failure") == 0 || std::strcmp(mode, "bad_error_failure") == 0)) {
        if (output != nullptr) {
            *output = {};
        }
        rodaq::plugin::abi::write_error(error, {});
        if (std::strcmp(mode, "bad_error_failure") == 0 && error != nullptr) {
            error->message.data = reinterpret_cast<std::uint8_t*>(1);
            error->message.size = 1;
        }
        return RODAQ_STATUS_UNAVAILABLE;
    }
    const auto code = NativeInstance::create(output, error);
    if (code != RODAQ_STATUS_OK) {
        return code;
    }
    output->vtable = &kFixtureVtable;
    if (mode != nullptr && std::strcmp(mode, "unknown") == 0) {
        output->vtable = &kPoisonedVtable;
        return 99;
    }
    if (mode != nullptr && std::strcmp(mode, "nondefault_failure") == 0) {
        output->vtable = &kPoisonedVtable;
        return RODAQ_STATUS_UNAVAILABLE;
    }
    if (mode != nullptr && std::strcmp(mode, "bad_error_ok") == 0) {
        output->vtable = &kPoisonedVtable;
        if (error != nullptr) {
            error->message.data = reinterpret_cast<std::uint8_t*>(1);
            error->message.size = 1;
        }
    }
    if (mode != nullptr && std::strcmp(mode, "malformed_success") == 0) {
        output->vtable = &kShortVtable;
    }
    return code;
}
