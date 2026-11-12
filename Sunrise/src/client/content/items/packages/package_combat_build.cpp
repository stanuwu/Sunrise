/** Combat taxonomy: the race of every actor class and the members of every combat label group. */

#include <algorithm>
#include <climits>
#include <limits>
#include <tuple>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808094.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
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

/** Logs one combat taxonomy read that failed and that a later slice retries. */
void report_failure(const char* reason, std::size_t value) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=combat_taxonomy result=fail reason=%s value=%zu",
                      reason,
                      value);
}

/** Settles the taxonomy unsupported over a refusal the installed data decides, and logs it. */
void settle_unsupported(const char* reason, std::size_t value) noexcept {
    state::build_data::settle_combat_taxonomy_unsupported();
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=combat_taxonomy result=unsupported reason=%s value=%zu",
                      reason,
                      value);
}

/**
 * Reads the race of every actor class from the table the investment root names. A table the
 * installed data refuses settles the taxonomy; a failed package read or allocation is retried.
 * @param races Receives the rows in ascending class order.
 */
[[nodiscard]] bool read_enemy_races(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    std::span<const std::byte> root,
                                    std::vector<std::byte>& blob,
                                    std::vector<combat::EnemyRace>& races) noexcept {
    std::uint32_t tag = 0;
    if (!tables::slot_tag(root, tables::kEnemyRaceTableSlot, tag)
        || tables::package_of(tag) == tables::kAbsentPackageId) {
        settle_unsupported("race_slot", tag);
        return false;
    }
    if (!reader::read_tag(source, scratch, tag, blob)) {
        report_failure("race_read", tag);
        return false;
    }
    const std::span<const std::byte> bytes{blob};
    tables::Array rows{};
    if (!tables::read_array(bytes,
                            tables::kEnemyRaceArrayDescriptor,
                            tables::kEnemyRaceRowClass,
                            tables::kEnemyRaceRowStride,
                            rows)
        || rows.count == 0 || rows.count > combat::kEnemyRaceCapacity) {
        settle_unsupported("race_table", rows.count);
        return false;
    }
    try {
        races.clear();
        for (std::size_t row = 0; row < rows.count; ++row) {
            const std::size_t at = rows.dataOffset + row * tables::kEnemyRaceRowStride;
            std::uint32_t classHash = 0;
            std::uint32_t race = 0;
            if (!tables::read(bytes, at + tables::kEnemyRaceClassOffset, classHash)
                || !tables::read(bytes, at + tables::kEnemyRaceOrdinalOffset, race)
                || race > std::uint32_t{(std::numeric_limits<std::uint8_t>::max)()}) {
                settle_unsupported("race_ordinal", race);
                return false;
            }
            races.push_back({classHash, static_cast<std::uint8_t>(race)});
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
 * class, so the table is found by its class; the installed packages carry exactly one. Any other
 * number of tables, or a table the installed data refuses, settles the taxonomy; a failed sweep,
 * package read or allocation is retried.
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
                            scanned)) {
        report_failure("label_sweep", sweep.matches);
        return false;
    }
    if (sweep.matches != 1) {
        settle_unsupported("label_sweep", sweep.matches);
        return false;
    }
    std::uint32_t tableClass = 0;
    if (!reader::read_tag(source, scratch, sweep.tag, blob, tableClass)) {
        report_failure("label_read", sweep.tag);
        return false;
    }
    const std::span<const std::byte> bytes{blob};
    tables::Array labels{};
    tables::Array members{};
    if (tableClass != generated::PerkLabelTable::kClass
        || !tables::read_array(bytes,
                               offsetof(generated::PerkLabelTable, labels),
                               generated::SU32HashElement::kClass,
                               sizeof(generated::SU32HashElement),
                               labels)
        || labels.count > combat::kLabelCapacity
        || !tables::read_array(bytes,
                               offsetof(generated::PerkLabelTable, compression.groups),
                               generated::SPerkLabelGroup::kClass,
                               sizeof(generated::SPerkLabelGroup),
                               members)) {
        settle_unsupported("label_table", blob.size());
        return false;
    }
    try {
        groups.clear();
        for (std::size_t group = 0; group < members.count; ++group) {
            generated::SPerkLabelGroup row{};
            if (!tables::read(bytes, members.dataOffset + group * sizeof row, row)) {
                settle_unsupported("label_group", group);
                return false;
            }
            // Bit n of a mask names label n, least significant bit first within each byte.
            for (std::size_t label = 0; label < combat::kLabelCapacity; ++label) {
                if ((row.labelMask[label / CHAR_BIT] & (1U << (label % CHAR_BIT))) == 0) {
                    continue;
                }
                const std::size_t labelAt =
                    labels.dataOffset + label * sizeof(generated::SU32HashElement);
                std::uint32_t labelHash = 0;
                if (label >= labels.count || !tables::read(bytes, labelAt, labelHash)) {
                    settle_unsupported("label_bit", label);
                    return false;
                }
                groups.push_back({row.groupHash, labelHash});
                if (groups.size() > combat::kLabelGroupCapacity) {
                    settle_unsupported("label_capacity", groups.size());
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

/** @return True when the combat taxonomy is published or can never be. */
bool combat_taxonomy_settled() noexcept {
    return state::build_data::combat_taxonomy_ready()
           || state::build_data::combat_taxonomy_unsupported();
}

/**
 * Publishes the race table and the label groups together. A failed read or cache write retries
 * next slice; rows the catalog refuses settle the taxonomy unsupported.
 */
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
    // Rows the catalog refuses, such as a repeated class or an empty group table, are the data's
    // own, so that refusal is final.
    const bool valid = state::build_data::valid_combat_taxonomy(races, groups);
    const bool published = valid && state::build_data::publish_combat_taxonomy(races, groups);
    const char* result = published ? "ok" : "fail";
    if (!valid) {
        state::build_data::settle_combat_taxonomy_unsupported();
        result = "unsupported";
    }
    core::log::writef(core::log::Channel::client,
                      published ? core::log::Level::info : core::log::Level::warn,
                      "ev=pkg stage=combat_taxonomy result=%s races=%zu label_groups=%zu",
                      result,
                      races.size(),
                      groups.size());
    return published;
}

} // namespace sunrise::client::content::items::packages
