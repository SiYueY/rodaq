#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rodaq/result.hpp>

#include "artifact_catalog.hpp"
#include "catalog_state.hpp"

namespace rodaq::artifact {

struct StartupScanReport {
    std::vector<std::shared_ptr<CatalogEntry>> entries;
    std::uint64_t total_physical_bytes{0};
    std::uint32_t cleaned_tmp{0};
    std::uint32_t cleaned_deleting{0};
    std::uint32_t corrupt_count{0};
    StatusCode health{StatusCode::Ok};
};

// Synchronous, bounded startup scan of <volume>/.rodaq/artifacts/:
// 1. Verifies volume marker and mount identity.
// 2. Enumerates .rodaq/artifacts/ (fail-closed if > options.max_artifacts).
// 3. Removes stale *.tmp and *.deleting trees with bounded depth/entry caps.
// 4. Parses each final <id>/manifest.json and verifies flat regular-file set
//    and exact physical sizes. Invalid/corrupt final artifacts remain in place
//    for operator diagnostics and are excluded from the normal Catalog.
Result<StartupScanReport> scan_volume(const CatalogOptions& options);

}  // namespace rodaq::artifact
