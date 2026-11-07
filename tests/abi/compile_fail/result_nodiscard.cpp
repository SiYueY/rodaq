#include <rodaq/result.hpp>

rodaq::Result<int> make_value() { return 7; }

int main() {
    make_value();
    return 0;
}
