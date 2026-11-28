#pragma once

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include <rodaq/plugin/provider.hpp>
#include <rodaq/plugin/types.hpp>

namespace rodaq::plugin {

namespace authoring {
template <typename Plugin>
struct Instance;
}

/** Borrowed Application logging callback valid for the enclosing Plugin Instance lifetime. */
class ApplicationLogger {
public:
    ApplicationLogger() = default;

    explicit ApplicationLogger(rodaq_application_callbacks_t callbacks) noexcept
    : callbacks_(callbacks) {}

    void log(LogLevel level, std::string_view message) const noexcept {
        if (callbacks_.log == nullptr) {
            return;
        }
        const auto size = message.size() > std::numeric_limits<std::uint32_t>::max()
                              ? std::numeric_limits<std::uint32_t>::max()
                              : static_cast<std::uint32_t>(message.size());
        const rodaq_string_t text{size == 0 ? nullptr : message.data(), size};
        callbacks_.log(callbacks_.context, static_cast<rodaq_log_level_t>(level), text);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return callbacks_.log != nullptr; }

private:
    rodaq_application_callbacks_t callbacks_{};
};

class PluginSetup {
public:
    PluginSetup(const PluginSetup&) = delete;
    PluginSetup& operator=(const PluginSetup&) = delete;
    PluginSetup(PluginSetup&&) = delete;
    PluginSetup& operator=(PluginSetup&&) = delete;

    [[nodiscard]] std::string_view config() const noexcept { return config_; }

    void log(LogLevel level, std::string_view message) const noexcept {
        logger_.log(level, message);
    }

    [[nodiscard]] ApplicationLogger logger() const noexcept { return logger_; }

    template <typename Interface, typename Implementation>
    Result<void> provide(Implementation& implementation) {
        return authoring::add_interface<Interface>(entries_, implementation);
    }

private:
    template <typename Plugin>
    friend struct authoring::Instance;

    PluginSetup(
        std::string_view config, rodaq_application_callbacks_t callbacks,
        std::vector<authoring::InterfaceEntry>& entries) noexcept
    : config_(config), logger_(callbacks), entries_(entries) {}

    std::string_view config_;
    ApplicationLogger logger_;
    std::vector<authoring::InterfaceEntry>& entries_;
};

}  // namespace rodaq::plugin
