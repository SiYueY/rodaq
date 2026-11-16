#include <rodaq/artifact/artifact_service.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "artifact_catalog.hpp"
#include "../persistence/decimal.hpp"

namespace rodaq {

Result<ArtifactId> parse_artifact_id(std::string_view text) {
    if (text.size() > 22 || text.substr(0, 2) != "a.") {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto value = persistence::unsigned_decimal(text.substr(2));
    return value.ok() && value.value() != 0
               ? Result<ArtifactId>(ArtifactId{value.value()})
               : Result<ArtifactId>(Status(StatusCode::InvalidArgument, {}));
}

Result<ArtifactFileId> parse_artifact_file_id(std::string_view text) {
    if (text.size() > 12 || text.substr(0, 2) != "f.") {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto value = persistence::unsigned_decimal(text.substr(2));
    if (!value.ok() || value.value() == 0 ||
        value.value() > std::numeric_limits<std::uint32_t>::max()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return ArtifactFileId{static_cast<std::uint32_t>(value.value())};
}

std::string to_string(ArtifactId id) { return "a." + std::to_string(id.value); }

std::string to_string(ArtifactFileId id) { return "f." + std::to_string(id.value); }

ArtifactService::ArtifactService(std::shared_ptr<artifact::ArtifactCatalog> catalog) noexcept
: catalog_(std::move(catalog)) {}

Result<std::unique_ptr<ArtifactService>> ArtifactService::create(
    std::shared_ptr<artifact::ArtifactCatalog> catalog) {
    if (!catalog) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        return std::unique_ptr<ArtifactService>(new ArtifactService(std::move(catalog)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ArtifactPage> ArtifactService::query_artifacts(const ArtifactQuery& query) const {
    return catalog_->query(query);
}

Result<ArtifactInfo> ArtifactService::get_artifact(ArtifactId id) const {
    return catalog_->get(id);
}

Result<ArtifactReadHandle> ArtifactService::acquire_read(ArtifactId id, ArtifactFileId file) {
    return catalog_->acquire(id, file);
}

}  // namespace rodaq
