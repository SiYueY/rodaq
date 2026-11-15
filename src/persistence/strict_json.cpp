#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace rodaq::persistence {
namespace {

// SAX avoids allocating an entire unbounded DOM before checking cardinality.
class StrictJson final : public nlohmann::json_sax<Json> {
public:
    explicit StrictJson(const JsonLimits& limits) : limits_(limits) {}

    bool null() override { return add(nullptr) != nullptr; }

    bool boolean(bool value) override { return add(value) != nullptr; }

    bool number_integer(number_integer_t value) override {
        return add(static_cast<double>(value)) != nullptr;
    }

    bool number_unsigned(number_unsigned_t value) override {
        return add(static_cast<double>(value)) != nullptr;
    }

    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && add(value) != nullptr;
    }

    bool string(string_t& value) override {
        return value.size() <= limits_.max_string_bytes && add(std::move(value)) != nullptr;
    }

    bool binary(binary_t&) override { return false; }

    bool start_object(std::size_t) override { return start(Json::object()); }

    bool key(string_t& key) override {
        if (frames_.empty() || !frames_.back().value->is_object() ||
            key.size() > limits_.max_string_bytes) {
            return false;
        }
        auto& frame = frames_.back();
        if (frame.pending_key || frame.value->size() >= limits_.max_object_members ||
            frame.value->contains(key)) {
            return false;
        }
        frame.key = std::move(key);
        frame.pending_key = true;
        return true;
    }

    bool end_object() override { return finish(true); }

    bool start_array(std::size_t) override { return start(Json::array()); }

    bool end_array() override { return finish(false); }

    bool parse_error(std::size_t, const std::string&, const Json::exception&) override {
        return false;
    }

    Json take() { return std::move(root_); }

private:
    struct Frame {
        Json* value;
        std::string key;
        bool pending_key{false};
    };

    Json* add(Json value) {
        if (nodes_ >= limits_.max_nodes) {
            return nullptr;
        }
        ++nodes_;
        if (frames_.empty()) {
            if (has_root_) {
                return nullptr;
            }
            has_root_ = true;
            root_ = std::move(value);
            return &root_;
        }
        auto& frame = frames_.back();
        if (frame.value->is_array()) {
            if (frame.value->size() >= limits_.max_array_elements) {
                return nullptr;
            }
            frame.value->push_back(std::move(value));
            return &frame.value->back();
        }
        if (!frame.pending_key) {
            return nullptr;
        }
        auto inserted = frame.value->emplace(std::move(frame.key), std::move(value));
        frame.pending_key = false;
        return &inserted.first.value();
    }

    bool start(Json value) {
        if (frames_.size() >= limits_.max_depth) {
            return false;
        }
        auto* inserted = add(std::move(value));
        if (inserted == nullptr) {
            return false;
        }
        frames_.push_back({inserted, {}, false});
        return true;
    }

    bool finish(bool object) {
        if (frames_.empty() || frames_.back().value->is_object() != object ||
            frames_.back().pending_key) {
            return false;
        }
        frames_.pop_back();
        return true;
    }

    const JsonLimits& limits_;
    Json root_;
    std::vector<Frame> frames_;
    std::size_t nodes_{0};
    bool has_root_{false};
};

}  // namespace

Result<Json> parse_json(std::string_view input, const JsonLimits& limits) {
    if (input.empty() || input.size() > limits.max_bytes || limits.max_depth == 0 ||
        limits.max_depth > 64 || limits.max_nodes == 0) {
        return Status(StatusCode::InvalidArgument, "JSON input/limit invalid");
    }
    try {
        StrictJson parser(limits);
        if (!Json::sax_parse(input.begin(), input.end(), &parser)) {
            return Status(StatusCode::InvalidArgument, "Invalid strict JSON or resource bound");
        }
        return parser.take();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Invalid strict JSON");
    }
}

}  // namespace rodaq::persistence
