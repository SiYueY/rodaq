#include "uuid.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <sys/random.h>

namespace rodaq::uuid {

bool nonzero(const Bytes& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
}

Result<Bytes> parse(std::string_view text) {
    if (text.size() != 36) {
        return Status(StatusCode::InvalidArgument, {});
    }
    Bytes value{};
    std::size_t output = 0;
    int high = -1;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return Status(StatusCode::InvalidArgument, {});
            }
            continue;
        }
        const char character = text[index];
        const int nibble = character >= '0' && character <= '9'   ? character - '0'
                           : character >= 'a' && character <= 'f' ? character - 'a' + 10
                                                                  : -1;
        if (nibble < 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (high < 0) {
            high = nibble;
        } else {
            value[output++] = static_cast<std::uint8_t>(high * 16 + nibble);
            high = -1;
        }
    }
    if (!nonzero(value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return value;
}

std::string format(const Bytes& value) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text += '-';
        }
        text += kHex[value[index] >> 4u];
        text += kHex[value[index] & 0xfu];
    }
    return text;
}

Result<Bytes> generate() {
    Bytes identity{};
    std::size_t filled = 0;
    while (filled < identity.size()) {
        const auto count = getrandom(identity.data() + filled, identity.size() - filled, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Status(StatusCode::Unavailable, {});
        }
        filled += static_cast<std::size_t>(count);
    }
    identity[6] = static_cast<std::uint8_t>((identity[6] & 0x0fu) | 0x40u);
    identity[8] = static_cast<std::uint8_t>((identity[8] & 0x3fu) | 0x80u);
    return identity;
}

}  // namespace rodaq::uuid
