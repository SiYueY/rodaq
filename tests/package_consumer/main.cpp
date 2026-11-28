#include <type_traits>

#include <rodaq/daq/runtime.hpp>
#include <rodaq/logging.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/config.hpp>
#include <rodaq/task/runtime.hpp>
#include <rodaq/task/task_service.hpp>

template <typename T, typename = void>
struct HasPublicOpen : std::false_type {};

template <typename T>
struct HasPublicOpen<T, std::void_t<decltype(&T::open)>> : std::true_type {};

template <typename T, typename = void>
struct HasPublicCreate : std::false_type {};

template <typename T>
struct HasPublicCreate<T, std::void_t<decltype(&T::create)>> : std::true_type {};

static_assert(!HasPublicOpen<rodaq::TaskService>::value);
static_assert(!HasPublicCreate<rodaq::ArtifactService>::value);
static_assert(!std::is_copy_constructible_v<rodaq::ArtifactService>);
static_assert(!HasPublicCreate<rodaq::SystemService>::value);
static_assert(HasPublicOpen<rodaq::DaqRuntime>::value);

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
