#include <rodaq/config/config.hpp>
#include <rodaq/runtime/runtime.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
              decltype(std::declval<rodaq::runtime::Runtime&>().start()), rodaq::Result<void>>);

int main() {
    rodaq::runtime::RuntimeConfig config;
    rodaq::runtime::Runtime runtime(config);
    const rodaq::Status primary_failure = runtime.primary_failure();
    return runtime.state() == rodaq::runtime::RuntimeState::Constructed && primary_failure.ok() ? 0
                                                                                                : 1;
}
