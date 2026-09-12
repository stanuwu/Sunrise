#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "catalog_manifest.h"

namespace sunrise::state::activity_sdk::generated_world::store {

/** Exact outcomes from reopening one manifest-owned scenario shard. */
enum class RecordLoadStatus : std::uint8_t {
    loaded,
    invalidIdentity,
    scenarioMismatch,
    missing,
    sourceMismatch,
    payloadMismatch,
    invalid,
};

/** @return Stable machine-readable text for one shard-record result. */
[[nodiscard]] const char* status_name(RecordLoadStatus value) noexcept;

/** Finds one unique record in a validated tag-sorted manifest section. */
[[nodiscard]] const manifest::Record* find_record(std::span<const manifest::Record> records,
                                                  std::uint32_t scenarioTag) noexcept;

/** Requires every manifest-owned shard to match the loaded SDK and current extraction format. */
[[nodiscard]] bool compatible_catalog(const wchar_t* manifestPath,
                                      std::wstring_view scenarioDirectory,
                                      const Digest& expectedSourceFingerprint,
                                      const manifest::SdkIdentity& expectedSdk) noexcept;

/** Builds the content-addressed path owned by one exact manifest record. */
[[nodiscard]] bool shard_path(std::wstring_view scenarioDirectory,
                              std::uint32_t scenarioTag,
                              const Digest& payloadSha256,
                              core::path::Buffer& output) noexcept;

/**
 * Reopens one exact manifest record without publishing it through another State namespace.
 * The returned snapshot is immutable and owns every decoded section for its full lifetime.
 */
[[nodiscard]] bool load_record(std::wstring_view scenarioDirectory,
                               std::uint32_t expectedScenarioTag,
                               std::string_view expectedScenarioName,
                               const Digest& expectedSourceFingerprint,
                               const manifest::Record& record,
                               std::shared_ptr<const build_data::scriptables::Snapshot>& output,
                               RecordLoadStatus& status) noexcept;

} // namespace sunrise::state::activity_sdk::generated_world::store
