#pragma once

#include <rodaq/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::plugin {

struct SemVer final {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> prerelease;
    std::vector<std::string> build;
    std::string identity;

    [[nodiscard]] bool stable() const noexcept { return prerelease.empty(); }
};

[[nodiscard]] Result<SemVer> parse_semver(std::string_view input);

// Negative if left has lower precedence, zero for equal precedence.
[[nodiscard]] int compare_semver_precedence(const SemVer& left, const SemVer& right);

}  // namespace rodaq::plugin
