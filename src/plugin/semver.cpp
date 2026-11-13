#include "semver.hpp"

#include <rodaq/status.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::plugin {
namespace {

Result<std::uint64_t> parse_numeric_identifier(std::string_view value, bool prerelease) {
    if (value.empty()) {
        return Status{StatusCode::InvalidArgument, "empty semantic version identifier"};
    }
    if (value.size() > 1u && value.front() == '0') {
        if (prerelease) {
            return Status{StatusCode::InvalidArgument, "numeric prerelease has a leading zero"};
        }
        return Status{StatusCode::InvalidArgument, "numeric version has a leading zero"};
    }
    std::uint64_t parsed = 0u;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return Status{StatusCode::InvalidArgument, "invalid semantic version number"};
    }
    return parsed;
}

bool numeric_identifier(std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char character) {
        return character >= '0' && character <= '9';
    });
}

Result<std::vector<std::string>> parse_identifiers(std::string_view value, bool prerelease) {
    std::vector<std::string> result;
    if (value.empty()) {
        return result;
    }

    std::size_t offset = 0u;
    while (offset <= value.size()) {
        const auto separator = value.find('.', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        const auto identifier = value.substr(offset, end - offset);
        if (identifier.empty()) {
            return Status{StatusCode::InvalidArgument, "empty semantic version identifier"};
        }
        if (prerelease && numeric_identifier(identifier) && identifier.size() > 1u &&
            identifier.front() == '0') {
            return Status{
                StatusCode::InvalidArgument, "numeric prerelease identifier has a leading zero"};
        }
        for (const char character : identifier) {
            const bool valid = (character >= '0' && character <= '9') ||
                               (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') || character == '-';
            if (!valid) {
                return Status{StatusCode::InvalidArgument, "invalid semantic version identifier"};
            }
        }
        result.emplace_back(identifier);
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1u;
    }
    return result;
}

}  // namespace

Result<SemVer> parse_semver(std::string_view input) {
    if (input.empty() || input.size() > 128u) {
        return Status{StatusCode::InvalidArgument, "semantic version length is invalid"};
    }

    const auto plus = input.find('+');
    const auto core_and_prerelease = input.substr(0u, plus);
    const auto build_part =
        plus == std::string_view::npos ? std::string_view{} : input.substr(plus + 1u);
    if (plus != std::string_view::npos &&
        (build_part.empty() || build_part.find('+') != std::string_view::npos)) {
        return Status{StatusCode::InvalidArgument, "invalid semantic version build metadata"};
    }

    const auto dash = core_and_prerelease.find('-');
    const auto core = core_and_prerelease.substr(0u, dash);
    const auto prerelease_part =
        dash == std::string_view::npos ? std::string_view{} : core_and_prerelease.substr(dash + 1u);
    if (dash != std::string_view::npos && prerelease_part.empty()) {
        return Status{StatusCode::InvalidArgument, "empty semantic version prerelease"};
    }

    const auto first_dot = core.find('.');
    if (first_dot == std::string_view::npos) {
        return Status{StatusCode::InvalidArgument, "semantic version requires three components"};
    }
    const auto second_dot = core.find('.', first_dot + 1u);
    if (second_dot == std::string_view::npos ||
        core.find('.', second_dot + 1u) != std::string_view::npos) {
        return Status{StatusCode::InvalidArgument, "semantic version requires three components"};
    }

    auto major = parse_numeric_identifier(core.substr(0u, first_dot), false);
    auto minor =
        parse_numeric_identifier(core.substr(first_dot + 1u, second_dot - first_dot - 1u), false);
    auto patch = parse_numeric_identifier(core.substr(second_dot + 1u), false);
    if (!major.ok() || !minor.ok() || !patch.ok()) {
        return Status{StatusCode::InvalidArgument, "invalid semantic version number"};
    }

    auto prerelease = parse_identifiers(prerelease_part, true);
    auto build = parse_identifiers(build_part, false);
    if (!prerelease.ok() || !build.ok()) {
        return Status{StatusCode::InvalidArgument, "invalid semantic version identifiers"};
    }

    return SemVer{
        major.value(),
        minor.value(),
        patch.value(),
        std::move(prerelease).value(),
        std::move(build).value(),
        std::string{input}};
}

int compare_semver_precedence(const SemVer& left, const SemVer& right) {
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    if (left.prerelease.empty() != right.prerelease.empty()) {
        return left.prerelease.empty() ? 1 : -1;
    }

    const auto size = std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t index = 0u; index < size; ++index) {
        const auto& left_identifier = left.prerelease[index];
        const auto& right_identifier = right.prerelease[index];
        if (left_identifier == right_identifier) {
            continue;
        }
        const bool left_numeric = numeric_identifier(left_identifier);
        const bool right_numeric = numeric_identifier(right_identifier);
        if (left_numeric && right_numeric) {
            if (left_identifier.size() != right_identifier.size()) {
                return left_identifier.size() < right_identifier.size() ? -1 : 1;
            }
            return left_identifier < right_identifier ? -1 : 1;
        }
        if (left_numeric != right_numeric) {
            return left_numeric ? -1 : 1;
        }
        return left_identifier < right_identifier ? -1 : 1;
    }
    if (left.prerelease.size() == right.prerelease.size()) {
        return 0;
    }
    return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

}  // namespace rodaq::plugin
