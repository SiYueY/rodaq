#pragma once

#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/status.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(RODAQ_PLUGIN_ID) || !defined(RODAQ_PLUGIN_NAME) || !defined(RODAQ_PLUGIN_VERSION)
#error "rodaq_register_plugin() or the build must define plugin metadata"
#endif

namespace rodaq::plugin::authoring {

inline rodaq_status_code_t code_from_status(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok:
            return RODAQ_STATUS_OK;
        case StatusCode::InvalidArgument:
            return RODAQ_STATUS_INVALID_ARGUMENT;
        case StatusCode::InvalidState:
            return RODAQ_STATUS_INVALID_STATE;
        case StatusCode::NotFound:
            return RODAQ_STATUS_NOT_FOUND;
        case StatusCode::Conflict:
            return RODAQ_STATUS_CONFLICT;
        case StatusCode::ResourceExhausted:
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        case StatusCode::DeadlineExceeded:
            return RODAQ_STATUS_DEADLINE_EXCEEDED;
        case StatusCode::Unavailable:
            return RODAQ_STATUS_UNAVAILABLE;
        case StatusCode::PermissionDenied:
            return RODAQ_STATUS_PERMISSION_DENIED;
        case StatusCode::DataLoss:
            return RODAQ_STATUS_DATA_LOSS;
        case StatusCode::Internal:
        default:
            return RODAQ_STATUS_INTERNAL;
    }
}

inline bool valid_error_buffer(const rodaq_error_t* error) noexcept {
    return interface_support::valid_error_buffer(error);
}

inline void clear_error(rodaq_error_t* error) noexcept { interface_support::clear_error(error); }

inline void write_error(rodaq_error_t* error, std::string_view message) noexcept {
    interface_support::write_error(error, message);
}

class OperationRegistry final {
public:
    OperationRegistry() = default;
    OperationRegistry(const OperationRegistry&) = delete;
    OperationRegistry& operator=(const OperationRegistry&) = delete;

    Result<void> add(
        std::string_view interface_id, std::uint16_t major, std::uint16_t minor, void* context,
        const void* vtable, std::uint32_t vtable_size) {
        if (frozen_) {
            return Status{StatusCode::InvalidState, "operation registry is frozen"};
        }
        if (interface_id.empty() || context == nullptr || vtable == nullptr ||
            vtable_size < sizeof(rodaq_struct_header_t) || interface_id.size() > UINT32_MAX ||
            !interface_support::valid_utf8(interface_id)) {
            return Status{StatusCode::InvalidArgument, "operation entry is invalid"};
        }
        for (const auto& entry : entries_) {
            if (entry.interface_id == interface_id && entry.major == major) {
                return Status{StatusCode::Conflict, "operation interface already registered"};
            }
        }
        entries_.push_back(
            Entry{std::string{interface_id}, major, minor, context, vtable, vtable_size});
        return {};
    }

    Result<void> freeze() {
        frozen_ = true;
        return {};
    }

    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    Result<rodaq_interface_reference_t> query(
        std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
        std::uint16_t max_minor) const {
        const Entry* best = nullptr;
        bool interface_exists = false;
        bool major_exists = false;
        for (const auto& entry : entries_) {
            if (entry.interface_id != interface_id) {
                continue;
            }
            interface_exists = true;
            if (entry.major != major) {
                continue;
            }
            major_exists = true;
            if (entry.minor < min_minor || min_minor > max_minor) {
                continue;
            }
            if (best == nullptr || entry.minor > best->minor) {
                best = &entry;
            }
        }
        if (best == nullptr) {
            if (!interface_exists) {
                return Status{StatusCode::NotFound, "operation interface was not found"};
            }
            if (!major_exists || min_minor > max_minor) {
                return Status{StatusCode::Unavailable, "operation version is incompatible"};
            }
            return Status{StatusCode::Unavailable, "operation version is incompatible"};
        }
        return rodaq_interface_reference_t{
            reinterpret_cast<rodaq_interface_context_t*>(best->context),
            reinterpret_cast<const rodaq_interface_vtable_t*>(best->vtable), best->vtable_size,
            rodaq_interface_version_t{major, std::min(best->minor, max_minor)}};
    }

private:
    struct Entry {
        std::string interface_id;
        std::uint16_t major = 0;
        std::uint16_t minor = 0;
        void* context = nullptr;
        const void* vtable = nullptr;
        std::uint32_t vtable_size = 0;
    };

    bool frozen_ = false;
    std::vector<Entry> entries_;
};

class PluginSetupFactory final {
public:
    [[nodiscard]] static PluginSetup create(
        std::string_view config, const rodaq_service_registry_reference_t* services,
        OperationRegistry* registry) noexcept {
        return PluginSetup{config, services, registry, &add_operation};
    }

private:
    [[nodiscard]] static Result<void> add_operation(
        void* builder, std::string_view interface_id, std::uint16_t major, std::uint16_t minor,
        void* context, const void* vtable, std::uint32_t vtable_size) {
        return static_cast<OperationRegistry*>(builder)->add(
            interface_id, major, minor, context, vtable, vtable_size);
    }
};

template <typename PluginType>
struct GeneratedContext {
    std::unique_ptr<PluginType> plugin = std::make_unique<PluginType>();
    OperationRegistry operations;
};

inline bool valid_string(rodaq_string_t string) noexcept {
    if (string.size == 0u) {
        return true;
    }
    if (string.data == nullptr) {
        return false;
    }
    return interface_support::valid_utf8(std::string_view{string.data, string.size});
}

inline std::string_view string_view_from_abi(rodaq_string_t string) noexcept {
    return valid_string(string) ? std::string_view{string.data, string.size} : std::string_view{};
}

template <typename PluginType>
rodaq_status_code_t configure(
    rodaq_plugin_context_t* opaque, const rodaq_service_registry_reference_t* services,
    const rodaq_config_t* config, rodaq_error_t* error) noexcept {
    if (!valid_error_buffer(error) || opaque == nullptr || services == nullptr ||
        config == nullptr || !valid_string(config->json)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);

    try {
        auto& context = *reinterpret_cast<GeneratedContext<PluginType>*>(opaque);
        if (context.operations.frozen()) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        auto setup = PluginSetupFactory::create(
            string_view_from_abi(config->json), services, &context.operations);
        auto result = context.plugin->configure(setup);
        if (!result.ok()) {
            write_error(error, result.status().message());
            return code_from_status(result.status().code());
        }
        auto frozen = context.operations.freeze();
        if (!frozen.ok()) {
            return code_from_status(frozen.status().code());
        }
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        write_error(error, "plugin configure allocation failed");
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        write_error(error, "plugin configure threw an exception");
        return RODAQ_STATUS_INTERNAL;
    }
}

template <typename PluginType>
rodaq_status_code_t query(
    rodaq_plugin_context_t* opaque, const rodaq_interface_query_t* interface_query,
    rodaq_interface_reference_t* output, rodaq_error_t* error) noexcept {
    if (output != nullptr) {
        *output = rodaq_interface_reference_t{};
    }
    if (!valid_error_buffer(error) || opaque == nullptr || interface_query == nullptr ||
        output == nullptr || !valid_string(interface_query->interface_id)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);

    try {
        auto& context = *reinterpret_cast<GeneratedContext<PluginType>*>(opaque);
        auto result = context.operations.query(
            string_view_from_abi(interface_query->interface_id), interface_query->major,
            interface_query->min_minor, interface_query->max_minor);
        if (!result.ok()) {
            write_error(error, result.status().message());
            return code_from_status(result.status().code());
        }
        *output = result.value();
        return RODAQ_STATUS_OK;
    } catch (...) {
        write_error(error, "plugin query threw an exception");
        return RODAQ_STATUS_INTERNAL;
    }
}

template <typename PluginType>
rodaq_status_code_t shutdown(
    rodaq_plugin_context_t* opaque, rodaq_deadline_t deadline, rodaq_error_t* error) noexcept {
    if (!valid_error_buffer(error) || opaque == nullptr ||
        (deadline.monotonic_deadline < 0 &&
         deadline.monotonic_deadline != RODAQ_DEADLINE_INFINITE)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);
    try {
        auto& context = *reinterpret_cast<GeneratedContext<PluginType>*>(opaque);
        auto result =
            context.plugin->shutdown(Deadline::from_nanoseconds(deadline.monotonic_deadline));
        if (!result.ok()) {
            write_error(error, result.status().message());
        }
        return code_from_status(result.status().code());
    } catch (const std::bad_alloc&) {
        write_error(error, "plugin shutdown allocation failed");
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        write_error(error, "plugin shutdown threw an exception");
        return RODAQ_STATUS_INTERNAL;
    }
}

template <typename PluginType>
void destroy(rodaq_plugin_context_t* opaque) noexcept {
    std::unique_ptr<GeneratedContext<PluginType>>{
        reinterpret_cast<GeneratedContext<PluginType>*>(opaque)}
        .reset();
}

template <typename PluginType>
rodaq_status_code_t create(rodaq_plugin_instance_t* instance, rodaq_error_t* error) noexcept {
    if (instance != nullptr) {
        *instance = rodaq_plugin_instance_t{};
    }
    if (!valid_error_buffer(error) || instance == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    clear_error(error);

    try {
        auto context = std::make_unique<GeneratedContext<PluginType>>();
        static const rodaq_plugin_instance_vtable_t vtable{
            rodaq_struct_header_t{sizeof(rodaq_plugin_instance_vtable_t)}, &configure<PluginType>,
            &query<PluginType>, &shutdown<PluginType>, &destroy<PluginType>};
        instance->context = reinterpret_cast<rodaq_plugin_context_t*>(context.release());
        instance->vtable = &vtable;
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        write_error(error, "plugin instance allocation failed");
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        write_error(error, "plugin constructor threw an exception");
        return RODAQ_STATUS_INTERNAL;
    }
}

}  // namespace rodaq::plugin::authoring

#define RODAQ_DEFINE_PLUGIN(PluginType)                                                           \
    static_assert(                                                                                \
        std::is_base_of<::rodaq::plugin::PluginBase, PluginType>::value,                          \
        "RODAQ_DEFINE_PLUGIN requires a PluginBase implementation");                              \
    static_assert(                                                                                \
        std::is_default_constructible<PluginType>::value,                                         \
        "RODAQ_DEFINE_PLUGIN requires a default-constructible Plugin");                           \
    static_assert(                                                                                \
        std::is_nothrow_destructible<PluginType>::value,                                          \
        "RODAQ_DEFINE_PLUGIN requires a non-throwing destructor");                                \
    namespace {                                                                                   \
    using RodaqGeneratedPlugin = PluginType;                                                      \
    }                                                                                             \
    extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {               \
        return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);                                                  \
    }                                                                                             \
    extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(     \
        void) {                                                                                   \
        static const rodaq_plugin_descriptor_t descriptor{                                        \
            rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)},                             \
            rodaq_string_t{RODAQ_PLUGIN_ID, static_cast<uint32_t>(sizeof(RODAQ_PLUGIN_ID) - 1u)}, \
            rodaq_string_t{                                                                       \
                RODAQ_PLUGIN_NAME, static_cast<uint32_t>(sizeof(RODAQ_PLUGIN_NAME) - 1u)},        \
            rodaq_string_t{                                                                       \
                RODAQ_PLUGIN_VERSION, static_cast<uint32_t>(sizeof(RODAQ_PLUGIN_VERSION) - 1u)}}; \
        static const rodaq_plugin_entry_t entry{                                                  \
            rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},                                  \
            []() -> const rodaq_plugin_descriptor_t* { return &descriptor; },                     \
            &::rodaq::plugin::authoring::create<RodaqGeneratedPlugin>};                           \
        return &entry;                                                                            \
    }
