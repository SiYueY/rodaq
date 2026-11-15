#include <rodaq/logging.hpp>
#include <rodaq/result.hpp>
#include <rodaq/task/config.hpp>

int main() {
    const rodaq::Result<int> value(17);
    const auto text = fmt::format("{}", value.value());
    const auto level = rodaq::logging::level();
    const auto rejected = rodaq::export_task_config({});
    return value.ok() && text == "17" && level == rodaq::logging::Level::Info && !rejected.ok() &&
                   rejected.status().code() == rodaq::StatusCode::InvalidArgument
               ? 0
               : 1;
}
