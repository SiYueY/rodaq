#include "native_resources.hpp"

#include <array>
#include <condition_variable>
#include <mutex>
#include <string>

#include <rclcpp/typesupport_helpers.hpp>
#include <rclcpp/version.h>

#include "ros2/schema.hpp"

namespace rodaq::ros2_acquisition {
namespace {

enum class Phase { Empty, Loading, Ready };

struct Library {
    Phase phase{Phase::Empty};
    std::string type;
    std::shared_ptr<rcpputils::SharedLibrary> owner;
};

struct Libraries {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Library, 256> slots;
};

struct Reservation {
    Reservation(Libraries& cache_in, Library& slot_in) noexcept : cache(cache_in), slot(slot_in) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    ~Reservation() {
        if (!armed) {
            return;
        }
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (slot.phase == Phase::Loading) {
            slot.type.clear();
            slot.phase = Phase::Empty;
            cache.changed.notify_all();
        }
    }

    Libraries& cache;
    Library& slot;
    bool armed{true};
};

Libraries& libraries() {
    // Explicit bounded process owner; never unload SDK dispatcher maps while
    // their native implementation handles may remain in other ROS entities.
    static auto* const owner = new Libraries;
    return *owner;
}

Result<std::shared_ptr<rcpputils::SharedLibrary>> load(std::string_view type) {
    try {
        auto library = rclcpp::get_typesupport_library(std::string(type), "rosidl_typesupport_cpp");
#if RCLCPP_VERSION_MAJOR >= 28
        const auto* support = rclcpp::get_message_typesupport_handle(
            std::string(type), "rosidl_typesupport_cpp", *library);
#else
        const auto* support =
            rclcpp::get_typesupport_handle(std::string(type), "rosidl_typesupport_cpp", *library);
#endif
        if (support == nullptr) {
            return Status(StatusCode::Unavailable, "ROS 2 message typesupport unavailable");
        }
        return library;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "ROS 2 typesupport allocation failed");
    } catch (const std::exception&) {
        return Status(StatusCode::Unavailable, "ROS 2 message typesupport unavailable");
    } catch (...) {
        return Status(StatusCode::Internal, "Unknown ROS 2 typesupport exception");
    }
}

}  // namespace

Result<void> Context::release_after_join() {
    if (is_valid()) {
        return Status(StatusCode::InvalidState, "ROS 2 Context must be shut down before release");
    }
    clean_up();
    return {};
}

Result<std::shared_ptr<rcpputils::SharedLibrary>> resident_typesupport(
    std::string_view type, plugin::Deadline deadline) {
    if (!ros2::valid_type_name(type)) {
        return Status(StatusCode::InvalidArgument, "Invalid resident ROS 2 type");
    }
    auto& cache = libraries();
    Library* selected = nullptr;
    {
        std::unique_lock<std::mutex> lock(cache.mutex);
        for (;;) {
            Library* available = nullptr;
            Library* loading = nullptr;
            for (auto& slot : cache.slots) {
                if (slot.phase == Phase::Empty) {
                    if (available == nullptr) {
                        available = &slot;
                    }
                } else if (slot.type == type) {
                    if (slot.phase == Phase::Ready) {
                        return slot.owner;
                    }
                    loading = &slot;
                    break;
                }
            }
            if (loading == nullptr) {
                if (available == nullptr) {
                    return Status(StatusCode::ResourceExhausted, "ROS 2 resident type limit");
                }
                selected = available;
                selected->type = type;
                selected->phase = Phase::Loading;
                break;
            }
            const auto completed = [&] {
                return loading->phase != Phase::Loading || loading->type != type;
            };
            if (deadline.is_infinite()) {
                cache.changed.wait(lock, completed);
            } else {
                auto remaining = deadline.remaining();
                if (!remaining.ok()) {
                    return remaining.status();
                }
                if (!cache.changed.wait_for(lock, remaining.value(), completed)) {
                    return Status(
                        StatusCode::DeadlineExceeded, "ROS 2 typesupport load remains owned");
                }
            }
        }
    }
    // Native loading/symbol resolution and rollback destruction are outside the
    // structural lock. The actual caller owns this single-flight reservation.
    Reservation reservation(cache, *selected);
    auto loaded = load(type);
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (loaded.ok()) {
            selected->owner = loaded.value();
            selected->phase = Phase::Ready;
        } else {
            selected->type.clear();
            selected->phase = Phase::Empty;
        }
        // Disarm before making the free slot visible to another loader. A
        // destructor that only checked Loading could erase its successor.
        reservation.armed = false;
        cache.changed.notify_all();
    }
    return loaded;
}

}  // namespace rodaq::ros2_acquisition
