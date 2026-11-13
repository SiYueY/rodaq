#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace rodaq::plugin::test_support {

inline void report_failure(std::string_view expression, std::string_view file, int line) noexcept {
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

}  // namespace rodaq::plugin::test_support

#define RODAQ_TEST_CHECK(expression)                                                        \
    do {                                                                                    \
        if (!(expression)) {                                                                \
            ::rodaq::plugin::test_support::report_failure(#expression, __FILE__, __LINE__); \
        }                                                                                   \
    } while (false)

#define RODAQ_TEST_REQUIRE(expression) RODAQ_TEST_CHECK(expression)
