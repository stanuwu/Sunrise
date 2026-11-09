/** Combat taxonomy: the race of every actor class and the members of every combat label group. */

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <tuple>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808094.h"
#include "../../../../state/build_data/combat/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace combat = state::build_data::combat;

/** The first label-table tag a class sweep reported and how many entries carried the class. */
struct LabelTableSweep {
    std::uint32_t tag{};
    std::size_t matches{};
};

/** Logs one refused combat taxonomy read. */
void report_failure(const char* reason, std::size_t value) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=combat_taxonomy result=fail reason=%s value=%zu",
                      reason,
                      value);
}

/** Copies one little-endian field the caller has already bounded. */
template <typename Value>
[[nodiscard]] Value field(std::span<const std::byte> blob, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return value;
}

/** @return True when an array's rows of one stride end inside the blob. */
[[nodiscard]] bool rows_fit(std::span<const std::byte> blob,
                            const tables::Array& array,
                            std::size_t stride) noexcept {
    return array.dataOffset <= blob.size()
           && array.count <= (blob.size() - array.dataOffset) / stride;
}

/**
 * Reads the race of every actor class from the table the investment root names.
 * @param races Receives the rows in ascending class order.
 */
[[nodiscard]] bool read_enemy_races(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    std::span<const std::byte> root,
                                    std::vector<std::byte>& blob,
                                    std::vector<combat::EnemyRace>& races) noexcept {
    std::uint32_t tag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kEnemyRaceTableSlot, tag) || tag == 0
        || !reader::read_tag(source, scratch, tag, blob)
        || !tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kEnemyRaceArrayDescriptor, rows)
        || rows.elementClass != tables::kEnemyRaceRowClass
        || rows.count > combat::kEnemyRaceCapacity
        || !rows_fit(blob, rows, tables::kEnemyRaceRowStride)) {
        report_failure("race_table", blob.size());
        return false;
    }
    const std::span<const std::byte> bytes{blob};
    try {
        races.clear();
        for (std::size_t row = 0; row < rows.count; ++row) {
            const std::size_t at = rows.dataOffset + row * tables::kEnemyRaceRowStride;
            const auto race = field<std::uint32_t>(bytes, at + tables::kEnemyRaceOrdinalOffset);
            if (race > std::uint32_t{(std::numeric_limits<std::uint8_t>::max)()}) {
                report_failure("race_ordinal", race);
                return false;
            }
            races.push_back({field<std::uint32_t>(bytes, at + tables::kEnemyRaceClassOffset),
                             static_cast<std::uint8_t>(race)});
        }
    } catch (...) {
        report_failure("race_storage", rows.count);
        return false;
    }
    std::sort(races.begin(), races.end(), [](const auto& left, const auto& right) noexcept {
        return left.classHash < right.classHash;
    });
    return true;
}

/** @param context Sweep result. @param tag One matching tag. @return Always true, to count all. */
bool collect_label_table_tag(void* context, std::uint32_t tag) noexcept {
    auto* sweep = static_cast<LabelTableSweep*>(context);
    if (sweep->matches == 0) {
        sweep->tag = tag;
    }
    ++sweep->matches;
    return true;
}

/**
 * Reads every group membership of the combat label table. A repack moves the tag but not the
 * class, so the table is found by its class; the installed packages carry exactly one.
 * @param groups Receives the rows in ascending group, then label, order.
 */
[[nodiscard]] bool read_label_groups(const reader::Source& source,
                                     reader::Scratch& scratch,
                                     std::vector<std::byte>& blob,
                                     std::vector<combat::LabelGroup>& groups) noexcept {
    LabelTableSweep sweep{};
    reader::ScanResult scanned{};
    if (!reader::scan_class(source,
                            generated::PerkLabelTable::kClass,
                            &collect_label_table_tag,
                            &sweep,
                            scanned)
        || sweep.matches != 1) {
        report_failure("label_sweep", sweep.matches);
        return false;
    }
    std::uint32_t tableClass = 0;
    tables::Array labels{};
    tables::Array members{};
    if (!reader::read_tag(source, scratch, sweep.tag, blob, tableClass)
        || tableClass != generated::PerkLabelTable::kClass
        || !tables::find_array_at(std::span<const std::byte>{blob},
                                  offsetof(generated::PerkLabelTable, labels),
                                  labels)
        || labels.elementClass != generated::SU32HashElement::kClass
        || labels.count > combat::kLabelCapacity
        || !rows_fit(blob, labels, sizeof(generated::SU32HashElement))
        || !tables::find_array_at(std::span<const std::byte>{blob},
                                  offsetof(generated::PerkLabelTable, compression.groups),
                                  members)
        || members.elementClass != generated::SPerkLabelGroup::kClass
        || !rows_fit(blob, members, sizeof(generated::SPerkLabelGroup))) {
        report_failure("label_table", blob.size());
        return false;
    }
    const std::span<const std::byte> bytes{blob};
    try {
        groups.clear();
        for (std::size_t group = 0; group < members.count; ++group) {
            const std::size_t at = members.dataOffset + group * sizeof(generated::SPerkLabelGroup);
            const auto groupHash =
                field<std::uint32_t>(bytes, at + offsetof(generated::SPerkLabelGroup, groupHash));
            const auto mask = field<decltype(generated::SPerkLabelGroup::labelMask)>(
                bytes, at + offsetof(generated::SPerkLabelGroup, labelMask));
            // Bit n of a mask names label n, least significant bit first within each byte.
            for (std::size_t label = 0; label < combat::kLabelCapacity; ++label) {
                if ((mask[label / CHAR_BIT] & (1U << (label % CHAR_BIT))) == 0) {
                    continue;
                }
                if (label >= labels.count) {
                    report_failure("label_bit", label);
                    return false;
                }
                const std::size_t labelAt =
                    labels.dataOffset + label * sizeof(generated::SU32HashElement);
                groups.push_back({groupHash, field<std::uint32_t>(bytes, labelAt)});
                if (groups.size() > combat::kLabelGroupCapacity) {
                    report_failure("label_capacity", groups.size());
                    return false;
                }
            }
        }
    } catch (...) {
        report_failure("label_storage", members.count);
        return false;
    }
    std::sort(groups.begin(), groups.end(), [](const auto& left, const auto& right) noexcept {
        return std::tie(left.groupHash, left.labelHash)
               < std::tie(right.groupHash, right.labelHash);
    });
    return true;
}

} // namespace

/** Publishes the race table and the label groups together; a refused read retries next slice. */
bool build_combat_taxonomy(const reader::Source& source,
                           reader::Scratch& scratch,
                           std::span<const std::byte> root,
                           std::vector<std::byte>& blob) noexcept {
    std::vector<combat::EnemyRace> races{};
    std::vector<combat::LabelGroup> groups{};
    if (!read_enemy_races(source, scratch, root, blob, races)
        || !read_label_groups(source, scratch, blob, groups)) {
        return false;
    }
    const bool published = state::build_data::publish_combat_taxonomy(races, groups);
    core::log::writef(core::log::Channel::client,
                      published ? core::log::Level::info : core::log::Level::warn,
                      "ev=pkg stage=combat_taxonomy result=%s races=%zu label_groups=%zu",
                      published ? "ok" : "fail",
                      races.size(),
                      groups.size());
    return published;
}

} // namespace sunrise::client::content::items::packages
