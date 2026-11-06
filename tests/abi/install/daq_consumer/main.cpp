#include <rodaq/config/config.hpp>
#include <rodaq/daq.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<rodaq::DAQ&>().start()), rodaq::Result<void>>);

int main() {
    rodaq::DAQConfig config;
    rodaq::DAQ daq(config);
    const rodaq::Status primary_failure = daq.primary_failure();
    return daq.state() == rodaq::DAQState::Constructed && primary_failure.ok() ? 0 : 1;
}
