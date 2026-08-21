#include <algorithm>
#include <atomic>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

namespace constants = state::build_data::constants;

/**
 * Sums one definition's declared contribution to a single stat row.
 * @param definitionIndex Native item or plug index.
 * @param row Stat table row.
 * @return The declared value, or 0 when the definition declares none.
 */
[[nodiscard]] std::int32_t definition_total(std::uint16_t definitionIndex,
                                            std::uint8_t row) noexcept {
    details::Definition detail{};
    if (definitionIndex == details::kUnavailableItemIndex
        || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
        return 0;
    }
    std::int32_t total = 0;
    const std::size_t stats =
        detail.statCount < detail.stats.size() ? detail.statCount : detail.stats.size();
    for (std::size_t entry = 0; entry < stats; ++entry) {
        if (detail.stats[entry].row == row) {
            total += detail.stats[entry].value;
        }
    }
    return total;
}

/**
 * Sums one equipped item's own roll plus every plug its sockets hold.
 * An armour piece keeps only a token value on its own definition, so counting the definition alone
 * understates it by its whole roll.
 * @param equipped Effective plug lanes.
 * @param row Stat table row.
 * @return The item's total for that row.
 */
[[nodiscard]] std::int32_t item_total(const Equipped& equipped, std::uint8_t row) noexcept {
    std::int32_t total = definition_total(equipped.definitionIndex, row);
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        total += definition_total(resolve_effective_plug(equipped, lane), row);
    }
    return total;
}

/**
 * Collects every stat row one equipped item or its plugs declare.
 * @param equipped Effective plug lanes.
 * @param count Occupied entries, advanced per distinct row.
 */
void collect_rows(const Equipped& equipped,
                  std::span<std::uint8_t> rows,
                  std::size_t& count) noexcept {
    const std::size_t lanes = equipped.laneCount + 1;
    for (std::size_t source = 0; source < lanes; ++source) {
        const std::uint16_t definitionIndex =
            source == 0 ? equipped.definitionIndex : resolve_effective_plug(equipped, source - 1);
        details::Definition detail{};
        if (definitionIndex == details::kUnavailableItemIndex
            || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
            continue;
        }
        const std::size_t stats =
            detail.statCount < detail.stats.size() ? detail.statCount : detail.stats.size();
        for (std::size_t entry = 0; entry < stats && count < rows.size(); ++entry) {
            const std::uint8_t row = detail.stats[entry].row;
            if (row != details::kEmptyStatRow
                && std::find(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(count), row)
                       == rows.begin() + static_cast<std::ptrdiff_t>(count)) {
                rows[count++] = row;
            }
        }
    }
}

/**
 * Appends one row to a stat table.
 * The client writes only rows that contribute, so a row worth 0 is omitted rather than written as
 * a live key asserting the stat is exactly 0.
 * @param row Stat table row.
 * @param value Signed total.
 * @param count Occupied rows, advanced when the row is written.
 */
void append(std::uint8_t row,
            std::int32_t value,
            std::array<layout::StatRow, layout::kStatRowCapacity>& table,
            std::size_t& count) noexcept {
    if (value <= 0 || count >= table.size()) {
        return;
    }
    table[count].key = static_cast<std::int8_t>(row);
    table[count].value = value;
    ++count;
}

/**
 * Fills one per-weapon table with every row that weapon and its plugs declare, ascending.
 * @param equipped Effective plug lanes of one weapon.
 * @param table Per-weapon stat table.
 */
void apply_weapon_table(const Equipped& equipped,
                        std::array<layout::StatRow, layout::kStatRowCapacity>& table) noexcept {
    std::array<std::uint8_t, layout::kStatRowCapacity> rows{};
    std::size_t rowCount = 0;
    collect_rows(equipped, rows, rowCount);
    std::sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(rowCount));
    std::size_t written = 0;
    for (std::size_t entry = 0; entry < rowCount; ++entry) {
        append(rows[entry], item_total(equipped, rows[entry]), table, written);
    }
}

/** Diagnostic latch: the constants are a boot-time domain, so one line settles their absence. */
std::atomic<bool> g_reportedMissingConstants{};

/** Reports once that the installed investment constants are not published. */
void report_missing_constants() noexcept {
    if (g_reportedMissingConstants.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=char_stats stage=constants result=absent");
}

} // namespace

/** Fills the character stat table and the three per-weapon stat tables. */
bool apply_stats(const family4::loadout::ResolvedInstances& instances,
                 std::int32_t light,
                 layout::Appearance& appearance) noexcept {
    constants::InvestmentConstants named{};
    if (!state::build_data::find_investment_constants(named)) {
        // Without the named rows there is no light row either, so the record cannot be built at
        // all. Report it once: the alternative is a silently empty stat table.
        report_missing_constants();
        return false;
    }
    std::array<std::uint8_t, constants::kCharacterStatRowCount> rows = named.characterStatRows;
    std::sort(rows.begin(), rows.end());

    std::size_t written = 0;
    append(named.lightStatRow, light, appearance.characterStats, written);
    for (const std::uint8_t row : rows) {
        std::int32_t total = 0;
        for (std::size_t index = 0; index < instances.itemCount; ++index) {
            details::Definition detail{};
            Equipped equipped{};
            if (resolve_equipped(instances.items[index], detail, equipped)) {
                total += item_total(equipped, row);
            }
        }
        append(row, total, appearance.characterStats, written);
    }

    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        const std::uint8_t slot = instances.items[index].equipmentSlot;
        const auto weapon = static_cast<std::size_t>(slot - kFirstWeaponSlot);
        if (slot < kFirstWeaponSlot || weapon >= appearance.weaponStats.size()) {
            continue;
        }
        details::Definition detail{};
        Equipped equipped{};
        if (resolve_equipped(instances.items[index], detail, equipped)) {
            apply_weapon_table(equipped, appearance.weaponStats[weapon]);
        }
    }
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
