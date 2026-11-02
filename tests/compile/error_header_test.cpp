#include <rodaq/error.hpp>

#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<rodaq::Error>);
static_assert(std::is_copy_constructible_v<rodaq::Error>);
static_assert(std::is_nothrow_move_constructible_v<rodaq::Error>);
static_assert(noexcept(std::declval<const rodaq::Error&>().code()));
static_assert(noexcept(std::declval<const rodaq::Error&>().native_code()));
static_assert(noexcept(std::declval<const rodaq::Error&>().domain()));
static_assert(noexcept(std::declval<const rodaq::Error&>().message()));

int main() { return 0; }
