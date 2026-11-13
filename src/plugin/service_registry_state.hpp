#pragma once

#include <rodaq/plugin/c/abi.h>
#include <rodaq/result.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::plugin {

class ServiceRegistryState final {
public:
    struct Entry {
        std::string interface_id;
        std::uint16_t major = 0;
        std::uint16_t minor = 0;
        void* context = nullptr;
        const void* vtable = nullptr;
        std::uint32_t vtable_size = 0;
        std::shared_ptr<const void> owner;
    };

    ServiceRegistryState() = default;
    ServiceRegistryState(const ServiceRegistryState&) = delete;
    ServiceRegistryState& operator=(const ServiceRegistryState&) = delete;
    ~ServiceRegistryState();

    [[nodiscard]] Result<void> add(Entry entry);
    [[nodiscard]] Result<void> freeze();
    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] Result<rodaq_interface_reference_t> query(
        std::string_view interface_id, std::uint16_t major, std::uint16_t min_minor,
        std::uint16_t max_minor) const;

    [[nodiscard]] static const rodaq_service_registry_vtable_t* vtable() noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    bool frozen_ = false;
};

[[nodiscard]] rodaq_service_registry_reference_t make_service_registry_reference(
    const std::shared_ptr<const ServiceRegistryState>& state) noexcept;

}  // namespace rodaq::plugin
