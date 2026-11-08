#include <cassert>

#define RODAQ_LOG_ACTIVE_LEVEL RODAQ_LOG_LEVEL_WARNING
#include <rodaq/logging.hpp>

namespace {

int side_effect_count = 0;

int next_value() {
    ++side_effect_count;
    return side_effect_count;
}

}  // namespace

int main() {
    RODAQ_LOG_INFO("pruned value={}", next_value());
    assert(side_effect_count == 0);

    RODAQ_LOG_WARNING("active value={}", next_value());
    assert(side_effect_count == 1);

    return 0;
}
