#include <rodaq/result.hpp>

#include <memory>
#include <type_traits>

static_assert(TL_EXPECTED_VERSION_MAJOR == 1);
static_assert(TL_EXPECTED_VERSION_MINOR == 3);
static_assert(TL_EXPECTED_VERSION_PATCH == 1);
static_assert(
    std::is_same_v<decltype(std::declval<rodaq::Result<int>&>().status()), const rodaq::Status&>);
static_assert(!std::is_default_constructible_v<rodaq::Result<int>>);
static_assert(std::is_default_constructible_v<rodaq::Result<void>>);
static_assert(std::is_copy_constructible_v<rodaq::Result<int>>);
static_assert(!std::is_copy_constructible_v<rodaq::Result<std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<rodaq::Result<std::unique_ptr<int>>>);
static_assert(std::is_same_v<decltype(std::declval<rodaq::Result<void>&>().value()), void>);

int main() { return 0; }
