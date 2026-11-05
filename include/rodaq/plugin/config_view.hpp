#ifndef RODAQ_PLUGIN_CONFIG_VIEW_HPP_
#define RODAQ_PLUGIN_CONFIG_VIEW_HPP_

#include <rodaq/config/config.hpp>
#include <rodaq/plugin/abi/config.h>
#include <rodaq/plugin/abi_support.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq {

using ConfigKind = rodaq_config_kind;

class ConfigView final {
public:
    ConfigView() = default;

    static Result<ConfigView> from_abi(rodaq_config_view view);

    ConfigKind kind() const noexcept;
    bool valid() const noexcept;
    std::size_t size() const noexcept;
    Result<ConfigView> find(std::string_view key) const;
    Result<ConfigView> at(std::size_t index) const;
    Result<std::string_view> key_at(std::size_t index) const;
    Result<bool> as_bool() const;
    Result<std::int64_t> as_int64() const;
    Result<double> as_double() const;
    Result<std::string_view> as_string() const;
    rodaq_config_view abi() const noexcept;

private:
    explicit ConfigView(rodaq_config_view view) noexcept;

    template <typename FindFunction>
    Result<ConfigView> child(FindFunction find_function, std::string_view key) const {
        if (!valid()) {
            return invalid();
        }
        rodaq_config_view child{};
        rodaq_error error{};
        const rodaq_status status =
            find_function(view_.context, rodaq_string_view{key.data(), key.size()}, &child, &error);
        return child_result(status, &error, child);
    }

    static Result<ConfigView> child_result(
        rodaq_status status, const rodaq_error* error, rodaq_config_view child);
    static nonstd::unexpected_type<Error> invalid();

    rodaq_config_view view_{};
};

}  // namespace rodaq

namespace rodaq::plugin {

using ConfigTree = rodaq::ConfigTree;

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_CONFIG_VIEW_HPP_
