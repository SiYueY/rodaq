#include <rodaq/task/config.hpp>

#include <array>
#include <iostream>
#include <string_view>

int main() {
    std::array<char, 1024 * 1024 + 1> bytes{};
    std::cin.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto size = static_cast<std::size_t>(std::cin.gcount());
    if (size == bytes.size()) {
        return 2;
    }
    const auto parsed = rodaq::parse_task_config(std::string_view(bytes.data(), size));
    if (!parsed.ok()) {
        std::cout << static_cast<int>(parsed.status().code()) << '\n';
        return 1;
    }
    const auto encoded = rodaq::export_task_config(parsed.value());
    if (!encoded.ok()) {
        std::cout << static_cast<int>(encoded.status().code()) << '\n';
        return 1;
    }
    std::cout << encoded.value() << '\n';
    return 0;
}
