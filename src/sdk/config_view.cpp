#include <rodaq/plugin/config_view.hpp>

namespace rodaq {

ConfigView::ConfigView(rodaq_config_view view) noexcept : view_(view) {}

Result<ConfigView> ConfigView::from_abi(rodaq_config_view view) {
    if (view.context == nullptr || view.vtable == nullptr ||
        view.vtable->protocol_version != RODAQ_CONFIG_PROTOCOL_VERSION ||
        view.vtable->struct_size < RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE ||
        view.vtable->kind == nullptr || view.vtable->size == nullptr ||
        view.vtable->find == nullptr || view.vtable->at == nullptr ||
        view.vtable->key_at == nullptr || view.vtable->as_bool == nullptr ||
        view.vtable->as_int64 == nullptr || view.vtable->as_double == nullptr ||
        view.vtable->as_string == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0, "ConfigView ABI is incomplete or incompatible");
    }
    return ConfigView(view);
}

ConfigKind ConfigView::kind() const noexcept {
    return valid() ? view_.vtable->kind(view_.context) : RODAQ_CONFIG_NULL;
}
bool ConfigView::valid() const noexcept {
    return view_.context != nullptr && view_.vtable != nullptr;
}
std::size_t ConfigView::size() const noexcept {
    const std::uint64_t size = valid() ? view_.vtable->size(view_.context) : 0;
    return static_cast<std::size_t>(size);
}
Result<ConfigView> ConfigView::find(std::string_view key) const {
    if (!valid()) {
        return invalid();
    }
    return child(view_.vtable->find, key);
}
Result<ConfigView> ConfigView::at(std::size_t index) const {
    if (!valid()) {
        return invalid();
    }
    rodaq_config_view child{};
    rodaq_status error{};
    const rodaq_status_code status =
        view_.vtable->at(view_.context, static_cast<std::uint64_t>(index), &child, &error);
    return child_result(status, &error, child);
}
Result<std::string_view> ConfigView::key_at(std::size_t index) const {
    if (!valid()) {
        return invalid();
    }
    rodaq_string_view key{};
    rodaq_status error{};
    const rodaq_status_code status =
        view_.vtable->key_at(view_.context, static_cast<std::uint64_t>(index), &key, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return std::string_view(key.data, static_cast<std::size_t>(key.size));
}
Result<bool> ConfigView::as_bool() const {
    if (!valid()) {
        return invalid();
    }
    rodaq_bool value = RODAQ_FALSE;
    rodaq_status error{};
    const rodaq_status_code status = view_.vtable->as_bool(view_.context, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return value == RODAQ_TRUE;
}
Result<std::int64_t> ConfigView::as_int64() const {
    if (!valid()) {
        return invalid();
    }
    std::int64_t value = 0;
    rodaq_status error{};
    const rodaq_status_code status = view_.vtable->as_int64(view_.context, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return value;
}
Result<double> ConfigView::as_double() const {
    if (!valid()) {
        return invalid();
    }
    double value = 0.0;
    rodaq_status error{};
    const rodaq_status_code status = view_.vtable->as_double(view_.context, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return value;
}
Result<std::string_view> ConfigView::as_string() const {
    if (!valid()) {
        return invalid();
    }
    rodaq_string_view value{};
    rodaq_status error{};
    const rodaq_status_code status = view_.vtable->as_string(view_.context, &value, &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    return std::string_view(value.data, static_cast<std::size_t>(value.size));
}
rodaq_config_view ConfigView::abi() const noexcept { return view_; }
Result<ConfigView> ConfigView::child_result(
    rodaq_status_code status, const rodaq_status* error, rodaq_config_view child) {
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, error, "sdk");
    }
    return from_abi(child);
}
Status ConfigView::invalid() {
    return Status(StatusCode::InvalidState, "sdk", 0, "ConfigView is invalid");
}

}  // namespace rodaq
