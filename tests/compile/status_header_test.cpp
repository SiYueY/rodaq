#include <rodaq/status.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_default_constructible_v<rodaq::Status>);
static_assert(std::is_copy_constructible_v<rodaq::Status>);
static_assert(std::is_nothrow_move_constructible_v<rodaq::Status>);
static_assert(noexcept(std::declval<const rodaq::Status&>().status_code()));
static_assert(noexcept(std::declval<const rodaq::Status&>().error_code()));
static_assert(noexcept(std::declval<const rodaq::Status&>().domain()));
static_assert(noexcept(std::declval<const rodaq::Status&>().message()));

int main() { return 0; }
