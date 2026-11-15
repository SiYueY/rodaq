#pragma once

#include <array>
#include <string_view>
#include <utility>

#include <rodaq/task/types.hpp>

namespace rodaq::task {

// Private persistent spelling tables. Unknown enumerators have no spelling.
template <typename Enum>
struct EnumNames;

template <>
struct EnumNames<DesiredState> {
    inline static constexpr auto values = std::array{
        std::pair{DesiredState::Stopped, std::string_view{"stopped"}},
        std::pair{DesiredState::Running, std::string_view{"running"}},
        std::pair{DesiredState::Paused, std::string_view{"paused"}}};
};

template <>
struct EnumNames<OperationKind> {
    inline static constexpr auto values = std::array{
        std::pair{OperationKind::Configure, std::string_view{"configure"}},
        std::pair{OperationKind::Start, std::string_view{"start"}},
        std::pair{OperationKind::Pause, std::string_view{"pause"}},
        std::pair{OperationKind::Resume, std::string_view{"resume"}},
        std::pair{OperationKind::Stop, std::string_view{"stop"}},
        std::pair{OperationKind::Reset, std::string_view{"reset"}},
        std::pair{OperationKind::Retire, std::string_view{"retire"}}};
};

template <>
struct EnumNames<OperationState> {
    inline static constexpr auto values = std::array{
        std::pair{OperationState::Accepted, std::string_view{"accepted"}},
        std::pair{OperationState::InProgress, std::string_view{"in_progress"}},
        std::pair{OperationState::Succeeded, std::string_view{"succeeded"}},
        std::pair{OperationState::PartiallySucceeded, std::string_view{"partially_succeeded"}},
        std::pair{OperationState::Failed, std::string_view{"failed"}},
        std::pair{OperationState::Superseded, std::string_view{"superseded"}}};
};

template <>
struct EnumNames<Retryability> {
    inline static constexpr auto values = std::array{
        std::pair{Retryability::Never, std::string_view{"never"}},
        std::pair{Retryability::AfterConditionChange, std::string_view{"after_condition_change"}},
        std::pair{Retryability::AfterBackoff, std::string_view{"after_backoff"}},
        std::pair{Retryability::AfterProcessRestart, std::string_view{"after_process_restart"}},
        std::pair{
            Retryability::ManualRecoveryRequired, std::string_view{"manual_recovery_required"}}};
};

template <>
struct EnumNames<FailureDomain> {
    inline static constexpr auto values = std::array{
        std::pair{FailureDomain::Request, std::string_view{"request"}},
        std::pair{FailureDomain::TaskControl, std::string_view{"task_control"}},
        std::pair{FailureDomain::Pipeline, std::string_view{"pipeline"}},
        std::pair{FailureDomain::Collector, std::string_view{"collector"}},
        std::pair{FailureDomain::Storage, std::string_view{"storage"}},
        std::pair{FailureDomain::Volume, std::string_view{"volume"}},
        std::pair{FailureDomain::PersistenceState, std::string_view{"persistence_state"}},
        std::pair{FailureDomain::Process, std::string_view{"process"}}};
};

template <>
struct EnumNames<DataImpact> {
    inline static constexpr auto values = std::array{
        std::pair{DataImpact::None, std::string_view{"none"}},
        std::pair{DataImpact::PreAdmissionOnly, std::string_view{"pre_admission_only"}},
        std::pair{DataImpact::AcceptedKnownFailed, std::string_view{"accepted_known_failed"}},
        std::pair{DataImpact::PersistenceUnknown, std::string_view{"persistence_unknown"}},
        std::pair{DataImpact::ArtifactQuarantined, std::string_view{"artifact_quarantined"}},
        std::pair{
            DataImpact::HistoricalArtifactLostOrCorrupt,
            std::string_view{"historical_artifact_lost_or_corrupt"}}};
};

template <typename Enum>
std::string_view enum_text(Enum value) noexcept {
    for (const auto& entry : EnumNames<Enum>::values) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return {};
}

template <typename Enum>
Result<Enum> parse_enum(std::string_view text) {
    for (const auto& entry : EnumNames<Enum>::values) {
        if (entry.second == text) {
            return entry.first;
        }
    }
    return Status(StatusCode::InvalidArgument, {});
}

}  // namespace rodaq::task
