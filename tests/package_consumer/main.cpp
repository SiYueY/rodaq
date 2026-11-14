#include <rodaq/logging.hpp>
#include <rodaq/result.hpp>

int main() {
    const rodaq::Result<int> value(17);
    const auto text = fmt::format("{}", value.value());
    const auto level = rodaq::logging::level();
    return value.ok() && text == "17" && level == rodaq::logging::Level::Info ? 0 : 1;
}
