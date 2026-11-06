#ifndef RODAQ_PLUGIN_SERVICE_CONTEXT_HPP_
#define RODAQ_PLUGIN_SERVICE_CONTEXT_HPP_

#include <cstddef>

namespace rodaq {

class ActivationState;

namespace plugin {

struct AcquisitionServiceContext {
    ActivationState* state = nullptr;
    std::size_t plugin_index = 0;
};

struct StorageServiceContext {
    ActivationState* state = nullptr;
};

}  // namespace plugin
}  // namespace rodaq

#endif  // RODAQ_PLUGIN_SERVICE_CONTEXT_HPP_
