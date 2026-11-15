#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rodaq/acquisition/c/interface.h>
#include <rodaq/acquisition/provider.hpp>
#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/operation.hpp>

namespace rodaq::acquisition {

using plugin::Deadline;
class Handle;

struct Interface {
    using Vtable = rodaq_acquisition_interface_vtable_t;
    using Handle = acquisition::Handle;
    static constexpr std::string_view kInterfaceId = RODAQ_ACQUISITION_INTERFACE_ID;
    static constexpr std::uint16_t kMajor = RODAQ_ACQUISITION_INTERFACE_MAJOR;
    static constexpr std::uint16_t kMinor = RODAQ_ACQUISITION_INTERFACE_MINOR;

    template <typename Implementation>
    using Provider = acquisition::Provider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, quiesce) + sizeof(Vtable::quiesce);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.prepare != nullptr && table.start != nullptr && table.quiesce != nullptr;
    }
};

/** No mutable business state: the backend and Core retain their own authorities. */
class Handle {
public:
    explicit Handle(plugin::OperationHandle<Interface> operation) noexcept;

    Result<void> prepare(const PrepareConfig& config, Deadline deadline);

    Result<void> start(Deadline deadline);

    Result<void> quiesce(Deadline deadline);

private:
    template <typename Function>
    Result<void> call(Function&& function);

    plugin::OperationHandle<Interface> operation_;
};

}  // namespace rodaq::acquisition
