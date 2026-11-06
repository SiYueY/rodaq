#ifndef RODAQ_BINDING_BINDING_STATE_HPP_
#define RODAQ_BINDING_BINDING_STATE_HPP_

#include "canonical/handle.hpp"
#include "canonical/registry.hpp"

#include <rodaq/daq_config.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

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

struct StreamBindingState {
    std::atomic<std::uint64_t> current_binding{0};
    std::mutex transition_mutex;
    std::condition_variable transition_condition;
    bool transition_active = false;
    std::uint64_t transition_token = 0;
};

class BindingState final {
public:
    BindingState(const DAQConfig& config, std::uint64_t activation_generation);

    BindingState(const BindingState&) = delete;
    BindingState& operator=(const BindingState&) = delete;
    BindingState(BindingState&&) = delete;
    BindingState& operator=(BindingState&&) = delete;

    Registry<BindingEntry, BindingHandle>& bindings() noexcept { return bindings_; }
    const Registry<BindingEntry, BindingHandle>& bindings() const noexcept { return bindings_; }
    std::vector<BindingHandle>& binding_handles() noexcept { return binding_handles_; }
    const std::vector<BindingHandle>& binding_handles() const noexcept { return binding_handles_; }

    void register_stream(StreamHandle stream);
    bool is_stream_registered(StreamHandle stream) const noexcept;
    StreamBindingState& stream_state(StreamHandle stream);
    std::uint64_t next_transition_id() noexcept { return next_transition_id_++; }

    Result<BindingHandle> create_binding(
        std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
        BindingLifecycle lifecycle);
    Result<BindingLifecycle> lifecycle(BindingHandle binding) const;
    Result<void> transition(
        BindingHandle binding, BindingLifecycle expected, BindingLifecycle desired);
    BindingHandle current_binding(StreamHandle stream) const noexcept;
    void set_current_binding(StreamHandle stream, BindingHandle binding) noexcept;
    bool retire_if_drained(BindingHandle binding) noexcept;

private:
    Registry<BindingEntry, BindingHandle> bindings_;
    std::vector<BindingHandle> binding_handles_;
    std::vector<std::unique_ptr<StreamBindingState>> streams_;
    std::uint64_t next_transition_id_ = 1;
};

}  // namespace rodaq

#endif  // RODAQ_BINDING_BINDING_STATE_HPP_
