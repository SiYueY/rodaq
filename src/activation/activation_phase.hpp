#ifndef RODAQ_ACTIVATION_ACTIVATION_PHASE_HPP_
#define RODAQ_ACTIVATION_ACTIVATION_PHASE_HPP_

namespace rodaq {

enum class ActivationPhase {
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_ACTIVATION_PHASE_HPP_
