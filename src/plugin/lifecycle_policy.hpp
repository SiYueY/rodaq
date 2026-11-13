#pragma once

#include <rodaq/plugin/deadline.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rodaq::plugin {

enum class LifecycleWorkKind { Refresh, Load, Create };

enum class LifecycleStage {
    Scheduling,
    CatalogScan,
    ManifestParse,
    CatalogBuild,
    CatalogPublish,
    NativeLoad,
    NativeCreate,
    FoundationValidation,
    NativeConfigure,
    OperationFreeze,
    OpenCommit,
    Cleanup
};

struct LifecyclePolicy final {
    DeadlineDuration scheduling = std::chrono::seconds{1};
    DeadlineDuration catalog_stage = std::chrono::seconds{10};
    DeadlineDuration load_stage = std::chrono::seconds{10};
    DeadlineDuration initialization_stage = std::chrono::seconds{5};
    DeadlineDuration post_abi_stage = std::chrono::seconds{1};
    DeadlineDuration close_stage = std::chrono::seconds{5};
};

class LifecycleWorkState final {
public:
    LifecycleWorkState(std::uint64_t id, LifecycleWorkKind kind) noexcept
    : id_(id), kind_(kind), stage_started_(std::chrono::steady_clock::now()) {}

    void set_stage(LifecycleStage stage) noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        stage_ = stage;
        stage_started_ = std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] LifecycleWorkKind kind() const noexcept { return kind_; }

    [[nodiscard]] LifecycleStage stage() const noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        return stage_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point stage_started() const noexcept {
        std::lock_guard<std::mutex> lock{mutex_};
        return stage_started_;
    }

private:
    std::uint64_t id_;
    LifecycleWorkKind kind_;
    mutable std::mutex mutex_;
    LifecycleStage stage_ = LifecycleStage::Scheduling;
    std::chrono::steady_clock::time_point stage_started_;
};

[[nodiscard]] inline DeadlineDuration lifecycle_stage_timeout(
    LifecycleWorkKind kind, LifecycleStage stage, const LifecyclePolicy& policy) noexcept {
    if (stage == LifecycleStage::Scheduling) {
        return policy.scheduling;
    }
    if (kind == LifecycleWorkKind::Refresh) {
        return policy.catalog_stage;
    }
    if (kind == LifecycleWorkKind::Load) {
        return policy.load_stage;
    }
    if (stage == LifecycleStage::Cleanup) {
        return policy.post_abi_stage;
    }
    return policy.initialization_stage;
}

}  // namespace rodaq::plugin
