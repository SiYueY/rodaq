#include <rodaq/result.hpp>

#include <memory>
#include <string>
#include <type_traits>

#if nsel_CONFIG_SELECT_EXPECTED != nsel_EXPECTED_NONSTD
#error "Result header must force the nonstd expected implementation"
#endif

static_assert(!nsel_USES_STD_EXPECTED);
static_assert(expected_lite_MAJOR == 0);
static_assert(expected_lite_MINOR == 10);
static_assert(expected_lite_PATCH == 0);
static_assert(std::is_same_v<rodaq::Result<int>, nonstd::expected<int, rodaq::Error>>);
static_assert(std::is_copy_constructible_v<rodaq::Result<int>>);
static_assert(!std::is_copy_constructible_v<rodaq::Result<std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<rodaq::Result<std::unique_ptr<int>>>);

int main() { return 0; }
