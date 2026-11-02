#include <stdexcept>

namespace {

void raw_callback() noexcept { throw std::runtime_error("raw noexcept violation"); }

}  // namespace

int main() {
    raw_callback();
    return 0;
}
