#include "test_support.hpp"

#include <string_view>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--expect-failure") {
        RODAQ_TEST_CHECK(false);
    }

    bool reached = false;
    if (reached) {
        RODAQ_TEST_CHECK(false);
    }

    RODAQ_TEST_CHECK(true);
    return EXIT_SUCCESS;
}
