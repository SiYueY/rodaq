#ifndef RODAQ_CONFIG_CONFIG_HPP_
#define RODAQ_CONFIG_CONFIG_HPP_

#include <rodaq/plugin/abi/config.h>
#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rodaq {

class ConfigNode;
class ConfigTree;

class ConfigNodeView final {
public:
    ConfigNodeView() = default;

    rodaq_config_kind kind() const noexcept;
    std::size_t size() const noexcept;

    Result<ConfigNodeView> find(std::string_view key) const;
    Result<ConfigNodeView> at(std::uint64_t index) const;
    Result<std::string_view> key_at(std::uint64_t index) const;

    Result<bool> as_bool() const;
    Result<std::int64_t> as_int64() const;
    Result<double> as_double() const;
    Result<std::string_view> as_string() const;

    rodaq_config_view abi_view() const noexcept;

private:
    explicit ConfigNodeView(std::shared_ptr<const ConfigNode> node);

    std::shared_ptr<const ConfigNode> node_;

    friend class ConfigTree;
};

class ConfigTree final {
public:
    using MapEntry = std::pair<std::string, ConfigTree>;

    ConfigTree() = default;

    static ConfigTree null();
    static ConfigTree boolean(bool value);
    static ConfigTree int64(std::int64_t value);
    static ConfigTree float64(double value);
    static ConfigTree string(std::string value);
    static Result<ConfigTree> map(std::vector<MapEntry> entries);
    static Result<ConfigTree> list(std::vector<ConfigTree> items);

    ConfigNodeView view() const noexcept;

private:
    explicit ConfigTree(std::shared_ptr<const ConfigNode> root);

    std::shared_ptr<const ConfigNode> root_;

    friend class ConfigNodeView;
};

}  // namespace rodaq

#endif  // RODAQ_CONFIG_CONFIG_HPP_
