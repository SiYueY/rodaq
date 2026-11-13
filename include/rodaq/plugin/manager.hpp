#pragma once

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/interface.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::plugin {

class PluginManagerState;
class PluginLibraryState;
class PluginInstanceState;
class PluginManagerTestingAccess;

using PluginInstanceId = std::uint64_t;

struct PluginManagerOptions {
    std::vector<std::filesystem::path> search_paths;
    std::size_t max_loaded_plugins = 32;
    std::size_t max_instances = 64;
    std::size_t max_config_size = 1024U * 1024U;
    DeadlineDuration shutdown_timeout = std::chrono::seconds{5};
};

struct PluginInfo {
    std::string plugin_id;
    std::string plugin_name;
    std::string plugin_version;
};

struct PluginArtifactInfo {
    std::string plugin_id;
    std::string plugin_version;
    std::string sha256;
};

enum class PluginManagerPhase { Running, ShuttingDown, Degraded, Stopped };

enum class PluginInstancePhase { Initializing, Open, Closing, Closed, Unresolved };

struct DiagnosticRecord {
    std::string plugin_id;
    std::string plugin_version;
    PluginInstanceId instance_id = 0;
    StatusCode status_code = StatusCode::Internal;
    std::string reason;
    std::string message;
    std::int64_t monotonic_timestamp = 0;
};

struct PluginDiagnostics {
    PluginManagerPhase phase = PluginManagerPhase::Running;
    bool restart_required = false;
    std::size_t loaded_plugin_count = 0;
    std::size_t live_instance_count = 0;
    std::size_t closing_instance_count = 0;
    std::size_t unresolved_instance_count = 0;
    std::size_t faulted_plugin_count = 0;
    std::vector<DiagnosticRecord> recent_errors;
};

enum class CloseWaitStatus { Completed, TimedOut };

class CloseFuture final {
public:
    CloseFuture() = delete;
    CloseFuture(const CloseFuture&) noexcept = default;
    CloseFuture& operator=(const CloseFuture&) noexcept = default;
    CloseFuture(CloseFuture&&) noexcept;
    CloseFuture& operator=(CloseFuture&&) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] CloseWaitStatus wait(Deadline deadline) const noexcept;
    [[nodiscard]] Result<void> result() const;

private:
    friend class PluginInstance;
    friend class PluginInstanceState;
    explicit CloseFuture(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;
};

class PluginInstance final {
public:
    PluginInstance() = delete;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
    PluginInstance(PluginInstance&&) noexcept;
    PluginInstance& operator=(PluginInstance&&) = delete;
    ~PluginInstance() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

    template <typename Interface>
    [[nodiscard]] Result<OperationHandle<Interface>> query() {
        auto raw_result = query_managed(
            Interface::kInterfaceId, Interface::kMajor, Interface::kMinMinor, Interface::kMinor);
        if (!raw_result.ok()) {
            return raw_result.status();
        }

        auto raw = std::move(raw_result).value();
        auto validation = interface_support::validate_typed_reference<Interface>(raw.reference_);
        if (!validation.ok()) {
            return validation.status();
        }
        OperationHandle<Interface> handle{
            raw.owner_, raw.reference_, raw.acquire_, raw.release_, raw.report_violation_};
        raw.release_admission();
        return handle;
    }

    [[nodiscard]] Result<void> close(Deadline deadline);
    [[nodiscard]] CloseFuture async_close() noexcept;
    [[nodiscard]] PluginInstancePhase phase() const noexcept;
    [[nodiscard]] PluginInstanceId id() const noexcept;

private:
    friend class PluginLibrary;
    friend class PluginInstanceState;

    class QueryAdmission final {
    public:
        QueryAdmission() = default;
        QueryAdmission(const QueryAdmission&) = delete;
        QueryAdmission& operator=(const QueryAdmission&) = delete;
        QueryAdmission(QueryAdmission&& other) noexcept;
        QueryAdmission& operator=(QueryAdmission&& other) noexcept;
        ~QueryAdmission();

        void release_admission() noexcept;

    private:
        friend class PluginInstance;
        friend class PluginInstanceState;

        QueryAdmission(
            std::shared_ptr<void> owner, rodaq_interface_reference_t reference,
            Result<void> (*acquire)(void*) noexcept, void (*release)(void*) noexcept,
            void (*report_violation)(void*, StatusCode, std::string_view) noexcept) noexcept;

        std::shared_ptr<void> owner_;
        rodaq_interface_reference_t reference_{};
        Result<void> (*acquire_)(void*) noexcept = nullptr;
        void (*release_)(void*) noexcept = nullptr;
        void (*report_violation_)(void*, StatusCode, std::string_view) noexcept = nullptr;
        bool admission_held_ = false;
    };

    explicit PluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept;

    [[nodiscard]] Result<QueryAdmission> query_managed(
        std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
        std::uint16_t max_minor);

    std::shared_ptr<PluginInstanceState> state_;
};

class PluginLibrary final {
public:
    PluginLibrary() = delete;
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    PluginLibrary(PluginLibrary&&) noexcept;
    PluginLibrary& operator=(PluginLibrary&&) noexcept;
    ~PluginLibrary() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const PluginInfo& info() const noexcept;
    [[nodiscard]] const PluginArtifactInfo& artifact() const noexcept;

    [[nodiscard]] Result<PluginInstance> create_instance(
        const ServiceRegistry& services, std::string_view config) const;

private:
    friend class PluginManager;
    explicit PluginLibrary(
        std::shared_ptr<PluginLibraryState> state,
        std::weak_ptr<PluginManagerState> manager) noexcept;

    std::shared_ptr<PluginLibraryState> state_;
    std::weak_ptr<PluginManagerState> manager_;
};

class PluginManager final {
public:
    static Result<PluginManager> create(PluginManagerOptions options);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) noexcept;
    PluginManager& operator=(PluginManager&&) noexcept;
    ~PluginManager() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<std::vector<PluginInfo>> plugins() const;
    [[nodiscard]] Result<PluginLibrary> load(std::string_view plugin_id);
    [[nodiscard]] Result<PluginLibrary> load(
        std::string_view plugin_id, std::string_view plugin_version);
    [[nodiscard]] Result<PluginDiagnostics> diagnostics() const;
    [[nodiscard]] Result<void> shutdown(Deadline deadline);

private:
    friend class PluginManagerTestingAccess;
    explicit PluginManager(std::shared_ptr<PluginManagerState> state) noexcept;

    std::shared_ptr<PluginManagerState> state_;
};

}  // namespace rodaq::plugin
