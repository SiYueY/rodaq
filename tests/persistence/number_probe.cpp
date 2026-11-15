#include "persistence/json.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.size() != 16) {
            return 2;
        }
        std::uint64_t bits = 0;
        const auto parsed = std::from_chars(line.data(), line.data() + line.size(), bits, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()) {
            return 2;
        }
        double value = 0;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        const auto encoded = rodaq::persistence::canonical_json(rodaq::persistence::Json(value));
        if (encoded.ok()) {
            std::cout << encoded.value() << '\n';
        } else if (encoded.status().code() == rodaq::StatusCode::InvalidArgument) {
            std::cout << "INVALID\n";
        } else {
            return 1;
        }
    }
}
