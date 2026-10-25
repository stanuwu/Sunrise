/**
 * Incident notifications for the SDK pack. An activity incident (message 19) targets one row of
 * the SObject table. A notification row (type code 0) displays its text when sent with the empty
 * incident header, and its name hash is also the string hash of that text in an installed string
 * bank, so the table and the banks together name every notification the client can show.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/activity_display_name_reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../state/activity_sdk/format.h"
#include "activity_sdk_native_pack_internal.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {
namespace {

namespace format = state::activity_sdk::format;
namespace display = middleware::content::packages::tables::activity_display_names;
namespace tables = middleware::content::packages::tables;
namespace incidents = incident_inventory;

/** The hash of the empty name: a row carrying it has no text to look up. */
constexpr std::uint32_t kEmptyNameHash = 0x811C9DC5U;

/** One SObject row that may carry an incident notification. */
struct Row final {
    std::uint32_t target{};
    std::uint32_t nameHash{};
    std::int32_t typeCode{};
};

/** Appends one installed string container to the caller's list. */
[[nodiscard]] bool collect_container(void* opaque, std::uint32_t tag) noexcept {
    if (opaque == nullptr || tag == 0 || tag == format::kAbsentIndex) {
        return false;
    }
    try {
        static_cast<std::vector<std::uint32_t>*>(opaque)->push_back(tag);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Reads the notification rows of the first installed SObject table that reads cleanly.
 * @return The table's tag, or zero when neither installed table could be read.
 */
[[nodiscard]] std::uint32_t read_notification_rows(PackageContext& packageContext,
                                                   std::vector<Row>& output) {
    output.clear();
    std::vector<std::byte> blob;
    for (const std::uint32_t tag :
         {tables::kSobjectTablePrimaryTag, tables::kSobjectTableAlternateTag}) {
        std::uint32_t classId = 0;
        if (!read_tag(&packageContext, tag, blob, classId)
            || blob.size() < tables::kSobjectFirstRow) {
            continue;
        }
        std::uint64_t rowCount = 0;
        std::memcpy(&rowCount, blob.data() + tables::kSobjectCountOffset, sizeof rowCount);
        if (rowCount == 0 || rowCount > format::kIncidentTargetCapacity
            || tables::kSobjectFirstRow
                       + static_cast<std::size_t>(rowCount) * tables::kSobjectRowStride
                   > blob.size()) {
            continue;
        }
        for (std::uint32_t target = 0; target < static_cast<std::uint32_t>(rowCount); ++target) {
            const std::size_t at = tables::kSobjectFirstRow + target * tables::kSobjectRowStride;
            Row row{target, 0, 0};
            std::memcpy(&row.nameHash, blob.data() + at + tables::kSobjectNameHashOffset, 4);
            std::memcpy(&row.typeCode, blob.data() + at + tables::kSobjectTypeCodeOffset, 4);
            if (row.typeCode == format::kIncidentNotificationTypeCode && row.nameHash != 0
                && row.nameHash != kEmptyNameHash) {
                output.push_back(row);
            }
        }
        return tag;
    }
    return 0;
}

void log_incidents(const incidents::Snapshot& snapshot, std::size_t containers) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_sdk_incidents table=%08X notifications=%u "
                                      "named=%zu containers=%zu",
                                      snapshot.tableTag,
                                      snapshot.notificationRows,
                                      snapshot.definitions.size(),
                                      containers);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            snapshot.tableTag == 0 ? core::log::Level::warn : core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

} // namespace

/**
 * Names every incident notification by its English text. A missing SObject table or a bank that
 * cannot be read leaves rows out rather than failing the pack: the module only grows less complete.
 */
bool build_incident_definitions(PackageContext& packageContext, incidents::Snapshot& output) {
    output = {};
    if (packageContext.source == nullptr) {
        return false;
    }
    std::vector<Row> rows;
    output.tableTag = read_notification_rows(packageContext, rows);
    output.notificationRows = static_cast<std::uint32_t>(rows.size());
    if (cancelled(packageContext.cancel, packageContext.cancelContext)) {
        return false;
    }
    std::vector<std::uint32_t> hashes;
    hashes.reserve(rows.size());
    for (const Row& row : rows) {
        hashes.push_back(row.nameHash);
    }
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());

    std::vector<std::uint32_t> containers;
    if (!hashes.empty()) {
        reader::ScanResult scan{};
        if (!reader::scan_class(*packageContext.source,
                                display::kStringContainerClass,
                                &collect_container,
                                &containers,
                                scan)) {
            return false;
        }
    }
    const display::Source strings{&packageContext, &read_localized_tag, 0, 0};
    std::vector<display::Reference> references;
    if (!display::locate(strings, containers, hashes, references)
        || cancelled(packageContext.cancel, packageContext.cancelContext)) {
        return false;
    }
    display::Snapshot names{};
    if (!references.empty() && !display::resolve(strings, references, names)) {
        return false;
    }
    if (names.names.size() != references.size()) {
        return false;
    }
    // References come back in hash order, so one binary search finds a row's text.
    for (const Row& row : rows) {
        const auto found = std::lower_bound(
            references.begin(), references.end(), row.nameHash, [](const auto& ref, auto hash) {
                return ref.stringHash < hash;
            });
        if (found == references.end() || found->stringHash != row.nameHash) {
            continue;
        }
        const display::Name& name =
            names.names[static_cast<std::size_t>(found - references.begin())];
        // Formatted strings resolve as authored-empty, and a blank text shows nothing: neither
        // gives the notification a name.
        const std::string_view text{name.value.data(), name.length};
        if (name.authoredEmpty || name.length >= incidents::kTextCapacity
            || std::all_of(text.begin(), text.end(), [](char c) {
                   return static_cast<unsigned char>(c) <= 0x20U;
               })) {
            continue;
        }
        incidents::Definition definition{};
        std::memcpy(definition.text.value.data(), name.value.data(), name.length);
        definition.text.length = name.length;
        definition.target = row.target;
        definition.nameHash = row.nameHash;
        definition.containerTag = found->containerTag;
        definition.typeCode = row.typeCode;
        output.definitions.push_back(definition);
    }
    log_incidents(output, containers.size());
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
