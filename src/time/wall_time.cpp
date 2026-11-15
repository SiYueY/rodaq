#include "wall_time.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <limits>
#include <new>

namespace rodaq::time {

static_assert(
    sizeof(time_t) >= sizeof(std::int64_t), "Wall-time persistence requires 64-bit time_t");

Result<std::string> format_wall_time(WallTime value) {
    constexpr std::int64_t kSecond = 1000000000;
    const auto count = value.time_since_epoch().count();
    auto seconds = count / kSecond;
    auto fraction = count % kSecond;
    if (fraction < 0) {
        --seconds;
        fraction += kSecond;
    }
    const auto native = static_cast<time_t>(seconds);
    tm calendar{};
    if (gmtime_r(&native, &calendar) == nullptr || calendar.tm_year < -1900 ||
        calendar.tm_year > 8099) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::array<char, 31> output{};
    const auto size = std::snprintf(
        output.data(), output.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour,
        calendar.tm_min, calendar.tm_sec, static_cast<long long>(fraction));
    if (size != 30) {
        return Status(StatusCode::Internal, {});
    }
    try {
        return std::string(output.data(), 30);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<WallTime> parse_wall_time(std::string_view text) {
    if (text.size() != 30 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text[29] != 'Z') {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto digits = [&](std::size_t offset, std::size_t count) -> int {
        int value = 0;
        for (std::size_t index = offset; index < offset + count; ++index) {
            if (text[index] < '0' || text[index] > '9') {
                return -1;
            }
            value = value * 10 + text[index] - '0';
        }
        return value;
    };
    tm calendar{};
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    const auto fraction = digits(20, 9);
    if (year < 1677 || year > 2262 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || fraction < 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    calendar.tm_year = year - 1900;
    calendar.tm_mon = month - 1;
    calendar.tm_mday = day;
    calendar.tm_hour = hour;
    calendar.tm_min = minute;
    calendar.tm_sec = second;
    errno = 0;
    const auto native = timegm(&calendar);
    // A successful libc call need not preserve errno. Only the documented
    // error return together with EOVERFLOW proves conversion failure.
    const auto conversion_error = errno;
    const auto seconds = static_cast<std::int64_t>(native);
    constexpr std::int64_t kSecond = 1000000000;
    constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    if ((native == static_cast<time_t>(-1) && conversion_error == EOVERFLOW) ||
        seconds < kMinimum / kSecond - 1 || seconds > kMaximum / kSecond) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::int64_t count = 0;
    if (seconds < 0) {
        const auto base = (seconds + 1) * kSecond;
        const auto remainder = kSecond - fraction;
        if (base < kMinimum + remainder) {
            return Status(StatusCode::InvalidArgument, {});
        }
        count = base - remainder;
    } else {
        const auto base = seconds * kSecond;
        if (base > kMaximum - fraction) {
            return Status(StatusCode::InvalidArgument, {});
        }
        count = base + fraction;
    }
    const WallTime value{std::chrono::nanoseconds{count}};
    auto canonical = format_wall_time(value);
    if (!canonical.ok()) {
        return std::move(canonical).status();
    }
    if (canonical.value() != text) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return value;
}

}  // namespace rodaq::time
