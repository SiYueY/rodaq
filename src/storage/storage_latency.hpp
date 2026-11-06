#ifndef RODAQ_STORAGE_STORAGE_LATENCY_HPP_
#define RODAQ_STORAGE_STORAGE_LATENCY_HPP_

namespace rodaq {

enum class StorageLatencyKind {
    Handoff,
    PayloadConsumed,
    PersistenceCompleted,
};

}  // namespace rodaq

#endif  // RODAQ_STORAGE_STORAGE_LATENCY_HPP_
