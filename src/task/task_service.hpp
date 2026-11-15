#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/plugin/manager.hpp>
#include <rodaq/task/task_service.hpp>

#include "task_store.hpp"
#include "../execution_lane.hpp"

namespace rodaq::task {

enum class AdmissionRole { Acquisition, Storage };

struct AllowedPlugin {
    std::string plugin_id;
    std::string plugin_version;
    AdmissionRole role{AdmissionRole::Acquisition};
};

enum class ServiceFileDomain { Directories, CreateReceipts, Task, StreamIdentity };
enum class MaterializationStep { Allocated, Materializing, Config, Identity, Control, Completed };

struct ServiceOptions {
    // Prepared private state root. Namespace initialization/reconciliation is a
    // startup activity; public query methods never scan or perform filesystem I/O.
    std::filesystem::path root;
    std::shared_ptr<plugin::PluginManager> plugin_manager;
    // Composition retains this scheduling domain through explicit shutdown;
    // Service destruction never joins threads or cancels accepted work.
    std::shared_ptr<ExecutionLane> create_lane;
    std::vector<AllowedPlugin> allowed_plugins;
    std::vector<std::string> resource_profiles;
    std::vector<std::string> output_roots;
    std::size_t max_tasks{8};
    std::size_t max_requests{32};
    std::size_t max_stream_identities{1024};
    TaskStorePolicy store;
    std::chrono::nanoseconds request_wait{std::chrono::seconds{30}};
    // Private deterministic persistence/crash seams, absent in production.
    std::function<int(ServiceFileDomain, persistence::FileStep)> file_faults;
    std::function<int(MaterializationStep)> materialization_faults;
};

}  // namespace rodaq::task
