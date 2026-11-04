#include <rodaq/config/config.hpp>
#include <rodaq/runtime/runtime.hpp>

int main() {
    rodaq::runtime::RuntimeConfig config;
    rodaq::runtime::Runtime runtime(config);
    return runtime.State() == rodaq::runtime::RuntimeState::Constructed ? 0 : 1;
}
