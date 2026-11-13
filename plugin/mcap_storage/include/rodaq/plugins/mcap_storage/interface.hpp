#pragma once

#include <rodaq/plugin/interface.hpp>

#include "rodaq/plugins/mcap_storage/c/interface.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq::plugins::mcap_storage {

using rodaq::plugin::OperationHandle;

struct StorageInterface {
    using Vtable = rodaq_reference_mcap_vtable_t;
    using Handle = OperationHandle<StorageInterface>;

    static constexpr std::string_view kInterfaceId = "com.rodaq.reference.mcap";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    template <typename Implementation>
    class Provider final {
    public:
        static const Vtable* vtable() noexcept {
            static const Vtable value{
                rodaq_struct_header_t{sizeof(Vtable)}, &open, &register_channel, &write, &close};
            return &value;
        }

    private:
        static rodaq_status_code_t open(
            rodaq_reference_mcap_context_t* context, rodaq_string_t path, rodaq_error_t*) noexcept {
            try {
                return reinterpret_cast<Implementation*>(context)->open_impl(path);
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static rodaq_status_code_t register_channel(
            rodaq_reference_mcap_context_t* context, rodaq_string_t topic,
            rodaq_string_t message_type, rodaq_string_t schema_encoding, rodaq_bytes_t schema,
            rodaq_string_t message_encoding, uint64_t* channel_id, rodaq_error_t*) noexcept {
            try {
                return reinterpret_cast<Implementation*>(context)->register_channel_impl(
                    topic, message_type, schema_encoding, schema, message_encoding, channel_id);
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static rodaq_status_code_t write(
            rodaq_reference_mcap_context_t* context, uint64_t channel_id, int64_t timestamp,
            rodaq_bytes_t payload, rodaq_error_t*) noexcept {
            try {
                return reinterpret_cast<Implementation*>(context)->write_impl(
                    channel_id, timestamp, payload);
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }

        static rodaq_status_code_t close(
            rodaq_reference_mcap_context_t* context, rodaq_error_t*) noexcept {
            try {
                return reinterpret_cast<Implementation*>(context)->close_impl();
            } catch (...) {
                return RODAQ_STATUS_INTERNAL;
            }
        }
    };

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(Vtable); }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->open != nullptr &&
               vtable->register_channel != nullptr && vtable->write != nullptr &&
               vtable->close != nullptr;
    }
};

inline Result<void> open(StorageInterface::Handle& handle, std::string_view path) {
    return handle.invoke([path](
                             rodaq_interface_context_t* context,
                             const StorageInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->open(
            reinterpret_cast<rodaq_reference_mcap_context_t*>(context),
            rodaq_string_t{path.data(), static_cast<std::uint32_t>(path.size())}, error);
    });
}

inline Result<std::uint64_t> register_channel(
    StorageInterface::Handle& handle, std::string_view topic, std::string_view message_type,
    std::string_view schema_encoding, std::string_view schema, std::string_view message_encoding) {
    std::uint64_t channel_id = 0u;
    auto result = handle.invoke([&](rodaq_interface_context_t* context,
                                    const StorageInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->register_channel(
            reinterpret_cast<rodaq_reference_mcap_context_t*>(context),
            rodaq_string_t{topic.data(), static_cast<std::uint32_t>(topic.size())},
            rodaq_string_t{message_type.data(), static_cast<std::uint32_t>(message_type.size())},
            rodaq_string_t{
                schema_encoding.data(), static_cast<std::uint32_t>(schema_encoding.size())},
            rodaq_bytes_t{
                reinterpret_cast<const std::uint8_t*>(schema.data()),
                static_cast<std::uint32_t>(schema.size())},
            rodaq_string_t{
                message_encoding.data(), static_cast<std::uint32_t>(message_encoding.size())},
            &channel_id, error);
    });
    if (!result.ok()) {
        return result.status();
    }
    return channel_id;
}

inline Result<void> write(
    StorageInterface::Handle& handle, std::uint64_t channel_id, std::int64_t timestamp,
    rodaq_bytes_t payload) {
    return handle.invoke([&](rodaq_interface_context_t* context,
                             const StorageInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->write(
            reinterpret_cast<rodaq_reference_mcap_context_t*>(context), channel_id, timestamp,
            payload, error);
    });
}

inline Result<void> close(StorageInterface::Handle& handle) {
    return handle.invoke([](rodaq_interface_context_t* context,
                            const StorageInterface::Vtable* vtable, rodaq_error_t* error) {
        return vtable->close(reinterpret_cast<rodaq_reference_mcap_context_t*>(context), error);
    });
}

}  // namespace rodaq::plugins::mcap_storage
