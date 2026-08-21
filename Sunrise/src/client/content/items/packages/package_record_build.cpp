#include <array>
#include <cstdio>
#include <cstring>

#include "../../../../core/logging/log.h"

#include "../../../../state/build_data/runtime.h"
#include "../../../../middleware/content/packages/tables/unlock_expression.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Reports where the record pass stopped, so a silent miss cannot look like a working claim. */
void report(const char* stage, unsigned long long detail) noexcept {
    std::array<char, 128> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=pkg stage=records result=%s detail=%llu", stage, detail);
    if (count > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** A record with no completion flag carries a non-positive slot, which addresses nothing. */
[[nodiscard]] constexpr bool addressable_slot(std::int16_t slot) noexcept {
    return slot > 0;
}


} // namespace

/**
 * Reads the records table and resolves each record's completion flag to a bank index.
 *
 * A record row carries the unlock slot of its completion flag, and a slot is not an array index:
 * the byte that feeds a slot sits at the row number of the mapping table whose destination is that
 * slot. Both tables are walked here so a claim can go straight from a record row to the index it
 * has to set.
 */
bool build_records(const reader::Source& source,
                   reader::Scratch& scratch,
                   std::span<const std::byte> root,
                   std::vector<std::byte>& blob,
                   std::span<state::build_data::records::Definition> output,
                   std::size_t& count) noexcept {
    namespace domain = state::build_data::records;
    count = 0;

    // The account flag mapping table, read first because the record rows are matched against it.
    std::uint32_t mapTag = 0;
    tables::Array mapRows{};
    if (!tables::slot_tag(root, tables::kUnlockFlagMapTableSlot, mapTag) || mapTag == 0
        || tables::package_of(mapTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, mapTag, blob)
        || !tables::find_array_at(std::span<const std::byte>{blob},
                                  tables::kAccountFlagMapDescriptor,
                                  mapRows)
        || mapRows.count == 0
        || mapRows.dataOffset
                   + static_cast<std::size_t>(mapRows.count) * tables::kUnlockMapRowStride
               > blob.size()) {
        report("flag_map_fail", mapTag);
        return false;
    }

    // Destination slot to mapping row. The first row wins, matching how a bank is addressed.
    constexpr std::size_t kSlotSpace = 32768;
    static_assert(domain::kUnavailableFlagIndex == 0xFFFFU);
    std::vector<std::uint16_t> indexBySlot{};
    indexBySlot.assign(kSlotSpace, domain::kUnavailableFlagIndex);
    for (std::uint64_t row = 0; row < mapRows.count; ++row) {
        const std::size_t at =
            mapRows.dataOffset + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride;
        std::int16_t slot = 0;
        std::memcpy(&slot, blob.data() + at + tables::kUnlockMapDestinationSlotOffset, sizeof slot);
        if (!addressable_slot(slot) || static_cast<std::size_t>(slot) >= kSlotSpace
            || row > domain::kUnavailableFlagIndex) {
            continue;
        }
        std::uint16_t& existing = indexBySlot[static_cast<std::size_t>(slot)];
        if (existing == domain::kUnavailableFlagIndex) {
            existing = static_cast<std::uint16_t>(row);
        }
    }

    // The account value mapping table, read while the blob is still free. A record names its
    // category's value slot, and that slot has to become an index the same way a flag slot does.
    std::uint32_t valueMapTag = 0;
    tables::Array valueMapRows{};
    std::vector<std::uint16_t> valueIndexBySlot{};
    if (tables::slot_tag(root, tables::kUnlockValueMapTableSlot, valueMapTag) && valueMapTag != 0
        && tables::package_of(valueMapTag) != tables::kAbsentPackageId
        && reader::read_tag(source, scratch, valueMapTag, blob)
        && tables::find_array_at(std::span<const std::byte>{blob},
                                 tables::kAccountValueMapDescriptor,
                                 valueMapRows)
        && valueMapRows.count != 0
        && valueMapRows.dataOffset
                   + static_cast<std::size_t>(valueMapRows.count) * tables::kUnlockMapRowStride
               <= blob.size()) {
        valueIndexBySlot.assign(kSlotSpace, domain::kUnavailableValueIndex);
        for (std::uint64_t row = 0; row < valueMapRows.count; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + valueMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            if (!addressable_slot(slot) || static_cast<std::size_t>(slot) >= kSlotSpace
                || row > domain::kUnavailableValueIndex) {
                continue;
            }
            std::uint16_t& existing = valueIndexBySlot[static_cast<std::size_t>(slot)];
            if (existing == domain::kUnavailableValueIndex) {
                existing = static_cast<std::uint16_t>(row);
            }
        }
    }

    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kRecordTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, tableTag, blob)
        || !tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kTableArrayDescriptor, rows)
        || rows.count == 0 || rows.count > output.size()
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kRecordRowStride
               > blob.size()) {
        report("record_table_fail", tableTag);
        return false;
    }

    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kRecordRowStride;
        std::int16_t slot = 0;
        std::memcpy(&slot,
                    blob.data() + at + tables::kRecordCompletionFlagOffset,
                    sizeof slot);
        std::uint32_t score = 0;
        std::memcpy(&score, blob.data() + at + tables::kRecordScoreOffset, sizeof score);
        domain::Definition& definition = output[static_cast<std::size_t>(row)];
        definition = {};
        definition.definitionIndex = static_cast<std::uint16_t>(row);
        // The shipped table tops out at 500, so anything wider is not a score and is dropped.
        definition.scoreValue = score <= 0xFFFFU ? static_cast<std::uint16_t>(score) : 0U;
        std::int16_t categorySlot = 0;
        if (!valueIndexBySlot.empty()
            && tables::expression_value_slot(std::span<const std::byte>{blob},
                                     at,
                                     tables::kRecordCategoryExpressionField,
                                     categorySlot)
            && addressable_slot(categorySlot)
            && static_cast<std::size_t>(categorySlot) < kSlotSpace) {
            definition.categoryValueIndex = valueIndexBySlot[static_cast<std::size_t>(categorySlot)];
        }
        if (addressable_slot(slot) && static_cast<std::size_t>(slot) < kSlotSpace) {
            definition.completionFlagIndex = indexBySlot[static_cast<std::size_t>(slot)];
        }
        ++count;
    }
    report("ok", static_cast<unsigned long long>(count));
    return count != 0;
}

} // namespace sunrise::client::content::items::packages
