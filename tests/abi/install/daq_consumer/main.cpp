#include <rodaq/config/config.hpp>
#include <rodaq/daq.hpp>
#include <rodaq/logging.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<rodaq::DAQ&>().start()), rodaq::Result<void>>);

int main() {
    rodaq::DAQConfig config;
    rodaq::DAQ daq(config);
    rodaq::Logger logger;
    RODAQ_LOG_INFO(logger, "installed consumer value={}", 42);
    const rodaq::Status primary_failure = daq.primary_failure();
    const auto control = logger.set_level(rodaq::LogLevel::Debug);
    return daq.state() == rodaq::DAQState::Constructed && primary_failure.ok() && !logger.valid() &&
                   control.status().code() == rodaq::StatusCode::InvalidState
               ? 0
               : 1;
}
