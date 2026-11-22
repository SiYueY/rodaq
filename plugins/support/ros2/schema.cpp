#include "schema.hpp"

#include <ament_index_cpp/get_resource.hpp>
#include <rodaq/plugin/abi.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <new>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rodaq::ros2 {
namespace {

bool token(std::string_view value) noexcept {
    const auto letter = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    return !value.empty() && value.size() <= 128 && letter(value.front()) &&
           std::all_of(value.begin(), value.end(), [&](unsigned char c) {
               return letter(c) || (c >= '0' && c <= '9') || c == '_';
           });
}

std::string_view trim(std::string_view value) noexcept {
    const auto begin = value.find_first_not_of(" \t\r");
    return begin == std::string_view::npos
               ? std::string_view{}
               : value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
}

Result<std::string> dependency_type(std::string_view type, std::string_view package) {
    std::string canonical;
    const auto slash = type.find('/');
    if (slash == std::string_view::npos) {
        canonical = std::string(package) + "/msg/" + std::string(type);
    } else if (type.find('/', slash + 1) == std::string_view::npos) {
        canonical =
            std::string(type.substr(0, slash)) + "/msg/" + std::string(type.substr(slash + 1));
    } else {
        canonical = type;
    }
    return valid_type_name(canonical)
               ? Result<std::string>(std::move(canonical))
               : Result<std::string>(Status(StatusCode::InvalidArgument, {}));
}

Result<std::filesystem::path> resource_path(std::string_view type, std::string_view extension) {
    const auto slash = type.find('/');
    const auto package = std::string(type.substr(0, slash));
    const auto relative = std::string(type.substr(slash + 1)) + std::string(extension);
    const auto name = relative.substr(relative.find_last_of('/') + 1);
    const auto category = relative.substr(0, relative.find('/'));
    std::string content;
    std::string prefix;
    if (!ament_index_cpp::get_resource("rosidl_interfaces", package, content, &prefix)) {
        return Status(StatusCode::Unavailable, "ROS interface resource is unavailable");
    }
    if (content.size() > 1024 * 1024) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (content.find('\0') != std::string::npos) {
        return Status(StatusCode::InvalidArgument, "Invalid ROS interface resource");
    }
    std::set<std::string> matches;
    bool exact = false;
    std::size_t entries = 0;
    std::string_view lines(content);
    while (!lines.empty()) {
        const auto end = lines.find('\n');
        const auto line = trim(lines.substr(0, end));
        if (++entries > 4096) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        if (line == relative) {
            exact = true;
        }
        // Some Jazzy packages place the registered file in a nested directory.
        // Select only a unique basename in the requested interface category.
        if (line.size() > name.size() && line.substr(0, category.size() + 1) == category + "/" &&
            line.substr(line.size() - name.size()) == name &&
            line[line.size() - name.size() - 1] == '/' &&
            line.find("..") == std::string_view::npos &&
            line.find('\\') == std::string_view::npos) {
            matches.emplace(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        lines.remove_prefix(end + 1);
    }
    if (exact) {
        return std::filesystem::path(prefix) / "share" / package / relative;
    }
    if (matches.size() > 1) {
        return Status(StatusCode::Conflict, "ROS interface basename is ambiguous");
    }
    if (matches.size() != 1) {
        return Status(StatusCode::Unavailable, "ROS interface definition is unavailable");
    }
    return std::filesystem::path(prefix) / "share" / package / *matches.begin();
}

class File {
public:
    explicit File(int fd) noexcept : fd_(fd) {}

    ~File() noexcept { ::close(fd_); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_;
};

Result<std::string> read_definition(const std::filesystem::path& path, std::size_t maximum) {
    const auto fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    File file(fd);
    struct stat facts {};
    if (::fstat(file.get(), &facts) != 0 || !S_ISREG(facts.st_mode) || facts.st_size < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    if (static_cast<std::uint64_t>(facts.st_size) > maximum) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    std::string text;
    std::array<char, 4096> block{};
    for (;;) {
        const auto size = ::read(file.get(), block.data(), block.size());
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status(StatusCode::Unavailable, {});
        }
        if (size == 0) {
            break;
        }
        if (static_cast<std::size_t>(size) > maximum - text.size()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        text.append(block.data(), static_cast<std::size_t>(size));
    }
    const rodaq_string_t view{text.data(), static_cast<std::uint32_t>(text.size())};
    if (!plugin::abi::valid_string(view) || text.find('\0') != std::string::npos) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return text;
}

std::string normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            result += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            result += text[i];
        }
    }
    while (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    result += '\n';
    return result;
}

std::string uncomment_idl(std::string_view line, bool& block) {
    std::string result;
    char quote = 0;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const auto c = line[i];
        const auto next = i + 1 < line.size() ? line[i + 1] : '\0';
        if (block) {
            if (c == '*' && next == '/') {
                block = false;
                ++i;
            }
        } else if (quote != 0) {
            result += c;
            if (!escaped && c == quote) {
                quote = 0;
            }
            escaped = !escaped && c == '\\';
        } else if (c == '/' && next == '/') {
            break;
        } else if (c == '/' && next == '*') {
            block = true;
            result += ' ';
            ++i;
        } else {
            result += c;
            if (c == '"' || c == '\'') {
                quote = c;
                escaped = false;
            }
        }
    }
    return result;
}

Result<std::set<std::string>> dependencies(
    std::string_view text, std::string_view package, bool idl, std::size_t maximum) {
    constexpr std::string_view kPrimitive[]{"bool",   "byte",  "char",   "float32", "float64",
                                            "int8",   "uint8", "int16",  "uint16",  "int32",
                                            "uint32", "int64", "uint64", "string",  "wstring"};
    std::set<std::string> result;
    bool comment = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto clean = idl ? uncomment_idl(text.substr(0, end), comment) : std::string{};
        auto line = trim(idl ? std::string_view(clean) : text.substr(0, end));
        std::string_view type;
        if (idl && !line.empty() && line.front() == '#') {
            const auto directive = trim(line.substr(1));
            if (directive.substr(0, 7) != "include") {
                if (end == std::string_view::npos) {
                    break;
                }
                text.remove_prefix(end + 1);
                continue;
            }
            auto value = trim(directive.substr(7));
            if (!value.empty() && (value.front() == '"' || value.front() == '<')) {
                const auto close = value.find(value.front() == '"' ? '"' : '>', 1);
                if (close != std::string_view::npos) {
                    value = value.substr(1, close - 1);
                    if (value.size() > 4 && value.substr(value.size() - 4) == ".idl") {
                        type = value.substr(0, value.size() - 4);
                    }
                }
            }
            if (type.empty()) {
                return Status(StatusCode::InvalidArgument, "Unsupported IDL include");
            }
        } else if (!idl && !line.empty() && line.front() != '#') {
            type = line.substr(0, line.find_first_of(" \t"));
            type = type.substr(0, type.find('['));
            type = type.substr(0, type.find("<="));
            if (std::find(std::begin(kPrimitive), std::end(kPrimitive), type) !=
                std::end(kPrimitive)) {
                type = {};
            }
        }
        if (!type.empty()) {
            auto canonical = dependency_type(type, package);
            if (!canonical.ok()) {
                return canonical.status();
            }
            result.insert(std::move(canonical.value()));
            if (result.size() > maximum) {
                return Status(StatusCode::ResourceExhausted, {});
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    if (comment) {
        return Status(StatusCode::InvalidArgument, "Unclosed IDL comment");
    }
    return result;
}

// Protocol reference: rosbag2 0.15.16 MessageDefinitionCache and 0.26.11
// LocalMessageDefinitionSource. This bounded traversal owns no long-lived cache.
struct Resolution {
    SchemaLimits limits;
    bool idl;
    std::set<std::string> seen;
    std::string output;
    std::size_t read_bytes{0};

    Result<void> append(std::string_view value) {
        if (value.size() > limits.max_bytes - output.size()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        output += value;
        return {};
    }

    Result<void> visit(const std::string& type, std::size_t depth, bool root) {
        if (seen.count(type) != 0) {
            return {};
        }
        if (depth > limits.max_depth || seen.size() == limits.max_definitions) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        seen.insert(type);
        const auto path = resource_path(type, idl ? ".idl" : ".msg");
        if (!path.ok()) {
            return path.status();
        }
        auto source = read_definition(path.value(), limits.max_bytes - read_bytes);
        if (!source.ok()) {
            return source.status();
        }
        read_bytes += source.value().size();
        const auto text = normalize(source.value());
        if (idl || !root) {
            auto label = type;
            if (!idl) {
                const auto category = label.find("/msg/");
                if (category != std::string::npos) {
                    label.erase(category, 4);
                }
            }
            const auto delimiter = (root ? std::string{} : "\n") + std::string(80, '=') +
                                   (idl ? "\nIDL: " : "\nMSG: ") + label + "\n";
            auto added = append(delimiter);
            if (!added.ok()) {
                return added;
            }
        }
        auto added = append(text);
        if (!added.ok()) {
            return added;
        }
        const auto children =
            dependencies(text, type.substr(0, type.find('/')), idl, limits.max_definitions);
        if (!children.ok()) {
            return children.status();
        }
        for (const auto& child : children.value()) {
            auto resolved = visit(child, depth + 1, false);
            if (!resolved.ok()) {
                return resolved;
            }
        }
        return {};
    }
};

}  // namespace

bool valid_type_name(std::string_view type) noexcept {
    if (type.size() > 256) {
        return false;
    }
    const auto package = type.find('/');
    if (package == std::string_view::npos || !token(type.substr(0, package))) {
        return false;
    }
    type.remove_prefix(package + 1);
    const auto category = type.find('/');
    const auto name = type.substr(0, category);
    if (category == std::string_view::npos ||
        (name != "msg" && name != "srv" && name != "action")) {
        return false;
    }
    type.remove_prefix(category + 1);
    for (;;) {
        const auto slash = type.find('/');
        if (!token(type.substr(0, slash))) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        type.remove_prefix(slash + 1);
    }
}

Result<Schema> resolve_schema(std::string_view type, SchemaLimits limits) {
    if (!valid_type_name(type) || limits.max_bytes == 0 || limits.max_bytes > 16 * 1024 * 1024 ||
        limits.max_definitions == 0 || limits.max_definitions > 4096 || limits.max_depth == 0 ||
        limits.max_depth > 64) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        for (const auto idl : {false, true}) {
            Resolution resolution{limits, idl, {}, {}, 0};
            auto resolved = resolution.visit(std::string(type), 1, true);
            if (resolved.ok()) {
                return Schema{
                    std::string(type), idl ? "ros2idl" : "ros2msg", std::move(resolution.output)};
            }
            if (idl || resolved.status().code() != StatusCode::Unavailable) {
                return resolved.status();
            }
        }
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::exception& error) {
        return Status(StatusCode::Unavailable, error.what());
    }
    return Status(StatusCode::Unavailable, {});
}

}  // namespace rodaq::ros2
