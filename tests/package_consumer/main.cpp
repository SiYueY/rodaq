#include <rodaq/daq/runtime.hpp>
#include <rodaq/logging.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/config.hpp>
#include <rodaq/task/runtime.hpp>
#include <rodaq/task/task_service.hpp>

int main() {
    const rodaq::Result<int> value(17);
    const auto text = fmt::format("{}", value.value());
    const auto level = rodaq::logging::level();
    const auto rejected = rodaq::export_task_config({});
    const rodaq::TaskRuntimeSnapshot runtime;
    const rodaq::SystemSnapshot system;
    const auto invalid_open = rodaq::DaqRuntime::open({});
    return value.ok() && text == "17" && level == rodaq::logging::Level::Info && !rejected.ok() &&
                   rejected.status().code() == rodaq::StatusCode::InvalidArgument &&
                   runtime.freshness == rodaq::SnapshotFreshness::NotObserved &&
                   system.freshness == rodaq::SnapshotFreshness::NotObserved &&
                   !invalid_open.ok() &&
                   invalid_open.status().code() == rodaq::StatusCode::InvalidArgument
               ? 0
               : 1;
}
