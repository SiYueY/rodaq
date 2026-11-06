#ifndef RODAQ_BINDING_BINDING_STATE_HPP_
#define RODAQ_BINDING_BINDING_STATE_HPP_

#include <canonical/handle.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rodaq {

enum class BindingLifecycle : std::uint32_t {
    Prepared = 1,
    Active = 2,
    Draining = 3,
    Retired = 4,
};

struct BindingEntry {
    StreamHandle stream;
    StreamDescriptorHandle descriptor;
    std::size_t plugin_index = 0;
    std::unique_ptr<std::atomic<std::uint32_t>> lifecycle;
    std::unique_ptr<std::atomic<std::uint64_t>> admission_inflight;
};

}  // namespace rodaq

#endif  // RODAQ_BINDING_BINDING_STATE_HPP_
