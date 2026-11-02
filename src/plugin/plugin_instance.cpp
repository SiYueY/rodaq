#include "plugin/plugin_instance.hpp"

#include "plugin/plugin_library.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace rodaq::plugin {

class PluginInstanceState {
public:
    PluginInstanceState(
        PluginLibraryLease library_lease, void* instance, void (*destroy)(void*) noexcept)
    : library_lease_(std::move(library_lease)), instance_(instance), destroy_(destroy) {}

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

private:
    PluginLibraryLease library_lease_;
    void* instance_;
    void (*destroy_)(void*) noexcept;
    std::atomic<bool> quarantined_{false};
};

std::shared_ptr<PluginInstanceState> PluginInstanceStateAccess::create(
    const PluginLibraryLease& library_lease, void* instance, void (*destroy)(void*) noexcept) {
    return std::make_shared<PluginInstanceState>(library_lease, instance, destroy);
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
