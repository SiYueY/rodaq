#include <rodaq/config/config.hpp>

#include <rodaq/plugin/abi_support.hpp>

#include <rodaq/error.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace rodaq {

struct ConfigMapEntry;

class ConfigNode final {
public:
    using Map = std::vector<ConfigMapEntry>;
    using List = std::vector<std::shared_ptr<const ConfigNode>>;
    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, Map, List>;

    explicit ConfigNode(Value node_value) : value(std::move(node_value)) {}

    Value value;
};

struct ConfigMapEntry {
    std::string key;
    std::shared_ptr<const ConfigNode> value;
};

namespace {

using NodePtr = std::shared_ptr<const ConfigNode>;

extern const rodaq_config_vtable kConfigVtable;

NodePtr make_node(ConfigNode::Value value) {
    return std::make_shared<const ConfigNode>(std::move(value));
}

Error config_error(ErrorCode code, std::string message) {
    return Error(code, "config", 0, std::move(message));
}

const ConfigNode* node_from_context(void* context) noexcept {
    return static_cast<const ConfigNode*>(context);
}

rodaq_config_kind abi_kind(const ConfigNode& node) noexcept {
    switch (node.value.index()) {
        case 0:
            return RODAQ_CONFIG_NULL;
        case 1:
            return RODAQ_CONFIG_BOOL;
        case 2:
            return RODAQ_CONFIG_INT64;
        case 3:
            return RODAQ_CONFIG_DOUBLE;
        case 4:
            return RODAQ_CONFIG_STRING;
        case 5:
            return RODAQ_CONFIG_MAP;
        case 6:
            return RODAQ_CONFIG_LIST;
        default:
            return RODAQ_CONFIG_NULL;
    }
}

std::uint64_t abi_size(const ConfigNode* node) noexcept {
    if (node == nullptr) {
        return 0;
    }
    if (const auto* entries = std::get_if<ConfigNode::Map>(&node->value)) {
        return static_cast<std::uint64_t>(entries->size());
    }
    if (const auto* items = std::get_if<ConfigNode::List>(&node->value)) {
        return static_cast<std::uint64_t>(items->size());
    }
    return 0;
}

rodaq_config_kind kind(void* context) noexcept {
    const ConfigNode* node = node_from_context(context);
    return node == nullptr ? RODAQ_CONFIG_NULL : abi_kind(*node);
}

std::uint64_t size(void* context) noexcept { return abi_size(node_from_context(context)); }

rodaq_status find(
    void* context, rodaq_string_view key_view, rodaq_config_view* out_child,
    rodaq_error* out_error) noexcept {
    if (out_child == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config find requires a non-null output view", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_child = {};

    const ConfigNode* node = node_from_context(context);
    if (node == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0, "Config view context is null", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const auto* entries = std::get_if<ConfigNode::Map>(&node->value);
    if (entries == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config find requires a MAP", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }

    std::string_view key;
    if (!parse_abi_string(key_view, &key)) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0, "Config key is not valid UTF-8", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const auto found = std::find_if(
        entries->begin(), entries->end(),
        [key](const ConfigMapEntry& entry) { return entry.key == key; });
    if (found == entries->end()) {
        assign_rodaq_error(
            RODAQ_STATUS_NOT_FOUND, "config", 0, "Config MAP key was not found", out_error);
        return RODAQ_STATUS_NOT_FOUND;
    }

    const auto* child = found->value.get();
    out_child->context = const_cast<ConfigNode*>(child);
    out_child->vtable = &kConfigVtable;
    return RODAQ_STATUS_OK;
}

const ConfigNode* child_at(const ConfigNode& node, std::uint64_t index) noexcept {
    if (const auto* entries = std::get_if<ConfigNode::Map>(&node.value)) {
        if (index >= entries->size()) {
            return nullptr;
        }
        return (*entries)[static_cast<std::size_t>(index)].value.get();
    }
    if (const auto* items = std::get_if<ConfigNode::List>(&node.value)) {
        if (index >= items->size()) {
            return nullptr;
        }
        return (*items)[static_cast<std::size_t>(index)].get();
    }
    return nullptr;
}

rodaq_status at(
    void* context, std::uint64_t index, rodaq_config_view* out_child,
    rodaq_error* out_error) noexcept {
    if (out_child == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0, "Config at requires a non-null output view",
            out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_child = {};

    const ConfigNode* node = node_from_context(context);
    if (node == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0, "Config view context is null", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    if (std::get_if<ConfigNode::Map>(&node->value) == nullptr &&
        std::get_if<ConfigNode::List>(&node->value) == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config at requires a MAP or LIST", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }

    const ConfigNode* child = child_at(*node, index);
    if (child == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_NOT_FOUND, "config", 0, "Config index is out of range", out_error);
        return RODAQ_STATUS_NOT_FOUND;
    }
    out_child->context = const_cast<ConfigNode*>(child);
    out_child->vtable = &kConfigVtable;
    return RODAQ_STATUS_OK;
}

rodaq_status key_at(
    void* context, std::uint64_t index, rodaq_string_view* out_key,
    rodaq_error* out_error) noexcept {
    if (out_key == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config key_at requires a non-null output key", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_key = {};

    const ConfigNode* node = node_from_context(context);
    if (node == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0, "Config view context is null", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const auto* entries = std::get_if<ConfigNode::Map>(&node->value);
    if (entries == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config key_at requires a MAP", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }
    if (index >= entries->size()) {
        assign_rodaq_error(
            RODAQ_STATUS_NOT_FOUND, "config", 0, "Config key index is out of range", out_error);
        return RODAQ_STATUS_NOT_FOUND;
    }

    const std::string& key = (*entries)[static_cast<std::size_t>(index)].key;
    out_key->data = key.data();
    out_key->size = static_cast<std::uint64_t>(key.size());
    return RODAQ_STATUS_OK;
}

template <typename Value>
const Value* scalar_from_context(void* context, rodaq_config_kind expected_kind) noexcept {
    const ConfigNode* node = node_from_context(context);
    if (node == nullptr || abi_kind(*node) != expected_kind) {
        return nullptr;
    }
    return std::get_if<Value>(&node->value);
}

rodaq_status as_bool(void* context, rodaq_bool* out_value, rodaq_error* out_error) noexcept {
    if (out_value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config as_bool requires a non-null output value", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_value = RODAQ_FALSE;
    const bool* value = scalar_from_context<bool>(context, RODAQ_CONFIG_BOOL);
    if (value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config value is not BOOL", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }
    *out_value = *value ? RODAQ_TRUE : RODAQ_FALSE;
    return RODAQ_STATUS_OK;
}

rodaq_status as_int64(void* context, std::int64_t* out_value, rodaq_error* out_error) noexcept {
    if (out_value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config as_int64 requires a non-null output value", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_value = 0;
    const std::int64_t* value = scalar_from_context<std::int64_t>(context, RODAQ_CONFIG_INT64);
    if (value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config value is not INT64", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }
    *out_value = *value;
    return RODAQ_STATUS_OK;
}

rodaq_status as_double(void* context, double* out_value, rodaq_error* out_error) noexcept {
    if (out_value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config as_double requires a non-null output value", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_value = 0.0;
    const double* value = scalar_from_context<double>(context, RODAQ_CONFIG_DOUBLE);
    if (value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config value is not DOUBLE", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }
    *out_value = *value;
    return RODAQ_STATUS_OK;
}

rodaq_status as_string(
    void* context, rodaq_string_view* out_value, rodaq_error* out_error) noexcept {
    if (out_value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "config", 0,
            "Config as_string requires a non-null output value", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_value = {};
    const std::string* value = scalar_from_context<std::string>(context, RODAQ_CONFIG_STRING);
    if (value == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_STATE, "config", 0, "Config value is not STRING", out_error);
        return RODAQ_STATUS_INVALID_STATE;
    }
    out_value->data = value->data();
    out_value->size = static_cast<std::uint64_t>(value->size());
    return RODAQ_STATUS_OK;
}

const rodaq_config_vtable kConfigVtable = {
    RODAQ_CONFIG_PROTOCOL_VERSION,
    RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE,
    &kind,
    &size,
    &find,
    &at,
    &key_at,
    &as_bool,
    &as_int64,
    &as_double,
    &as_string,
};

rodaq_config_view make_abi_view(const ConfigNode* node) noexcept {
    return rodaq_config_view{const_cast<ConfigNode*>(node), &kConfigVtable};
}

}  // namespace

ConfigNodeView::ConfigNodeView(std::shared_ptr<const ConfigNode> node) : node_(std::move(node)) {}

rodaq_config_kind ConfigNodeView::kind() const noexcept {
    return node_ == nullptr ? RODAQ_CONFIG_NULL : abi_kind(*node_);
}

std::size_t ConfigNodeView::size() const noexcept {
    const std::uint64_t value = abi_size(node_.get());
    if (value > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(value);
}

Result<ConfigNodeView> ConfigNodeView::find(std::string_view key) const {
    if (node_ == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config view is empty"));
    }
    const auto* entries = std::get_if<ConfigNode::Map>(&node_->value);
    if (entries == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config find requires a MAP"));
    }
    if (!rodaq::is_valid_utf8_without_nul(key)) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidArgument, "Config key is not valid UTF-8"));
    }
    const auto found = std::find_if(
        entries->begin(), entries->end(),
        [key](const ConfigMapEntry& entry) { return entry.key == key; });
    if (found == entries->end()) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::NotFound, "Config MAP key was not found"));
    }
    return ConfigNodeView(found->value);
}

Result<ConfigNodeView> ConfigNodeView::at(std::uint64_t index) const {
    if (node_ == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config view is empty"));
    }
    if (std::get_if<ConfigNode::Map>(&node_->value) == nullptr &&
        std::get_if<ConfigNode::List>(&node_->value) == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config at requires a MAP or LIST"));
    }
    const ConfigNode* child = child_at(*node_, index);
    if (child == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::NotFound, "Config index is out of range"));
    }
    return ConfigNodeView(std::shared_ptr<const ConfigNode>(node_, child));
}

Result<std::string_view> ConfigNodeView::key_at(std::uint64_t index) const {
    if (node_ == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config view is empty"));
    }
    const auto* entries = std::get_if<ConfigNode::Map>(&node_->value);
    if (entries == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config key_at requires a MAP"));
    }
    if (index >= entries->size()) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::NotFound, "Config key index is out of range"));
    }
    return std::string_view((*entries)[static_cast<std::size_t>(index)].key);
}

Result<bool> ConfigNodeView::as_bool() const {
    const bool* value = node_ == nullptr ? nullptr : std::get_if<bool>(&node_->value);
    if (value == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config value is not BOOL"));
    }
    return *value;
}

Result<std::int64_t> ConfigNodeView::as_int64() const {
    const std::int64_t* value =
        node_ == nullptr ? nullptr : std::get_if<std::int64_t>(&node_->value);
    if (value == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config value is not INT64"));
    }
    return *value;
}

Result<double> ConfigNodeView::as_double() const {
    const double* value = node_ == nullptr ? nullptr : std::get_if<double>(&node_->value);
    if (value == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config value is not DOUBLE"));
    }
    return *value;
}

Result<std::string_view> ConfigNodeView::as_string() const {
    const std::string* value = node_ == nullptr ? nullptr : std::get_if<std::string>(&node_->value);
    if (value == nullptr) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidState, "Config value is not STRING"));
    }
    return std::string_view(*value);
}

rodaq_config_view ConfigNodeView::AbiView() const noexcept { return make_abi_view(node_.get()); }

ConfigTree::ConfigTree(std::shared_ptr<const ConfigNode> root) : root_(std::move(root)) {}

ConfigTree ConfigTree::Null() { return ConfigTree(make_node(std::monostate{})); }

ConfigTree ConfigTree::Boolean(bool value) { return ConfigTree(make_node(value)); }

ConfigTree ConfigTree::Int64(std::int64_t value) { return ConfigTree(make_node(value)); }

ConfigTree ConfigTree::Double(double value) { return ConfigTree(make_node(value)); }

ConfigTree ConfigTree::String(std::string value) { return ConfigTree(make_node(std::move(value))); }

Result<ConfigTree> ConfigTree::Map(std::vector<MapEntry> entries) {
    ConfigNode::Map node_entries;
    node_entries.reserve(entries.size());
    for (MapEntry& entry : entries) {
        if (!rodaq::is_valid_utf8_without_nul(entry.first)) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::InvalidArgument, "Config MAP key is not valid UTF-8"));
        }
        const bool duplicate = std::any_of(
            node_entries.begin(), node_entries.end(),
            [&entry](const auto& existing) { return existing.key == entry.first; });
        if (duplicate) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::Conflict, "Config MAP contains a duplicate key"));
        }
        if (entry.second.root_ == nullptr) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::InvalidArgument, "Config MAP value is empty"));
        }
        node_entries.push_back(
            ConfigMapEntry{std::move(entry.first), std::move(entry.second.root_)});
    }
    return ConfigTree(make_node(std::move(node_entries)));
}

Result<ConfigTree> ConfigTree::List(std::vector<ConfigTree> items) {
    ConfigNode::List node_items;
    node_items.reserve(items.size());
    for (ConfigTree& item : items) {
        if (item.root_ == nullptr) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::InvalidArgument, "Config LIST item is empty"));
        }
        node_items.push_back(std::move(item.root_));
    }
    return ConfigTree(make_node(std::move(node_items)));
}

ConfigNodeView ConfigTree::view() const noexcept { return ConfigNodeView(root_); }

}  // namespace rodaq
