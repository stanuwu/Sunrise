#include "../account/public_profiles.h"
#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** Before character selection, encoders use the first character's banner. */
int selected_slot() noexcept {
    for (std::size_t slot = 0; slot < g_session.selected.size(); ++slot) {
        if (g_session.selected[slot]) {
            return static_cast<int>(slot);
        }
    }
    return 0;
}

/** Rejects rows outside the destination bank before assigning their scalar value. */
template <typename T, std::size_t N>
bool assign(std::array<T, N>& values,
            std::size_t slot,
            std::size_t lane,
            std::int32_t value) noexcept {
    if (slot >= values.size() || lane != 0) {
        return false;
    }
    if constexpr (std::is_same_v<T, std::uint8_t>) {
        if (value < 0 || value > (std::numeric_limits<T>::max)()) {
            return false;
        }
    }
    values[slot] = static_cast<T>(value);
    return true;
}

bool assign(unlocks::ProgressionBank& values,
            std::size_t slot,
            std::size_t lane,
            std::int32_t value) noexcept {
    if (slot >= values.size() || lane >= unlocks::kProgressionLaneCount) {
        return false;
    }
    values[slot][lane] = value;
    return true;
}

/** Sparse rows decode into the exact native bank without changing untouched lanes. */
bool assign(unlocks::Table& table,
            Bank bank,
            std::size_t slot,
            std::size_t lane,
            std::int32_t value) noexcept {
    switch (bank) {
    case Bank::accountFlags:
        return assign(table.accountFlags, slot, lane, value);
    case Bank::profileFlags:
        return assign(table.profileFlags, slot, lane, value);
    case Bank::characterFlags:
        return assign(table.characterFlags, slot, lane, value);
    case Bank::objectiveValues:
        return assign(table.objectiveValues, slot, lane, value);
    case Bank::characterObjectFlags:
        return assign(table.characterObjectFlags, slot, lane, value);
    case Bank::characterObjectValues:
        return assign(table.characterObjectValues, slot, lane, value);
    case Bank::accountProgressions:
        return assign(table.accountProgressions, slot, lane, value);
    case Bank::characterProgressions:
        return assign(table.characterProgressions, slot, lane, value);
    }
    return false;
}

/** Only changed lanes reach SQLite, and zeros remove their sparse rows. */
template <typename T, std::size_t N>
bool write_bank(Statement& rows,
                Statement& remove,
                int owner,
                Bank bank,
                const std::array<T, N>& before,
                const std::array<T, N>& values) noexcept {
    const auto write = [&](std::size_t slot, std::size_t lane, std::int32_t value) noexcept {
        return value == 0 ? remove.write(owner, bank, slot, lane)
                          : rows.write(owner, bank, slot, lane, value);
    };
    for (std::size_t slot = 0; slot < values.size(); ++slot) {
        if constexpr (std::is_integral_v<T>) {
            if (values[slot] != before[slot] && !write(slot, 0, values[slot])) {
                return false;
            }
        } else {
            for (std::size_t lane = 0; lane < values[slot].size(); ++lane) {
                if (values[slot][lane] != before[slot][lane]
                    && !write(slot, lane, values[slot][lane])) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

/** Returns account banks and the selected character's banks under one database lock. */
bool read_unlocks(unlocks::Table& output, int characterSlot) noexcept {
    const std::lock_guard lock(g_mutex);
    output = {};
    const int selected = characterSlot < 0 ? selected_slot() : characterSlot;
    if (selected >= static_cast<int>(kCharacterCapacity)) {
        return false;
    }
    Statement rows("SELECT character_slot,bank,slot,lane,value FROM unlocks "
                   "WHERE character_slot=-1 OR character_slot=?");
    if (!rows.parameters(selected)) {
        return false;
    }
    int result = rows.step();
    while (result == SQLITE_ROW) {
        int owner = 0;
        Bank bank{};
        std::size_t slot = 0;
        std::size_t lane = 0;
        std::int32_t value = 0;
        if (!rows.columns(owner, bank, slot, lane, value)
            || ((owner == -1 || owner == selected) && !assign(output, bank, slot, lane, value))) {
            output = {};
            return false;
        }
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Other characters' banks survive writes to the selected character. */
bool write_unlocks(const unlocks::Table& value, int characterSlot) noexcept {
    Transaction transaction;
    const int selected = characterSlot < 0 ? selected_slot() : characterSlot;
    if (selected >= static_cast<int>(kCharacterCapacity)) {
        return false;
    }
    unlocks::Table before;
    if (!transaction.ready() || !read_unlocks(before, selected)) {
        return false;
    }
    Statement rows("INSERT OR REPLACE INTO unlocks VALUES (?,?,?,?,?)");
    Statement remove("DELETE FROM unlocks WHERE character_slot=? AND bank=? AND slot=? AND lane=?");
    return write_bank(rows, remove, -1, Bank::accountFlags, before.accountFlags, value.accountFlags)
           && write_bank(
               rows, remove, -1, Bank::profileFlags, before.profileFlags, value.profileFlags)
           && write_bank(rows,
                         remove,
                         selected,
                         Bank::characterFlags,
                         before.characterFlags,
                         value.characterFlags)
           && write_bank(rows,
                         remove,
                         -1,
                         Bank::objectiveValues,
                         before.objectiveValues,
                         value.objectiveValues)
           && write_bank(rows,
                         remove,
                         selected,
                         Bank::characterObjectFlags,
                         before.characterObjectFlags,
                         value.characterObjectFlags)
           && write_bank(rows,
                         remove,
                         selected,
                         Bank::characterObjectValues,
                         before.characterObjectValues,
                         value.characterObjectValues)
           && write_bank(rows,
                         remove,
                         -1,
                         Bank::accountProgressions,
                         before.accountProgressions,
                         value.accountProgressions)
           && write_bank(rows,
                         remove,
                         selected,
                         Bank::characterProgressions,
                         before.characterProgressions,
                         value.characterProgressions)
           && transaction.commit();
}

/** Scalar readers use the indexed row instead of loading every unlock bank. */
bool read_unlock(Bank bank, std::uint16_t slot, std::int32_t& value) noexcept {
    const std::lock_guard lock(g_mutex);
    value = 0;
    const bool character = bank == Bank::characterFlags || bank == Bank::characterObjectFlags
                           || bank == Bank::characterObjectValues
                           || bank == Bank::characterProgressions;
    Statement row(
        "SELECT value FROM unlocks WHERE character_slot=? AND bank=? AND slot=? AND lane=0");
    if (!row.parameters(character ? selected_slot() : -1, bank, slot)) {
        return false;
    }
    const int result = row.step();
    return result == SQLITE_DONE || (result == SQLITE_ROW && row.column(0, value));
}

/** One scalar write updates only its indexed bank entry. */
bool write_unlock(Bank bank, std::uint16_t slot, std::int32_t value) noexcept {
    const std::lock_guard lock(g_mutex);
    const bool character = bank == Bank::characterFlags || bank == Bank::characterObjectFlags
                           || bank == Bank::characterObjectValues
                           || bank == Bank::characterProgressions;
    const int owner = character ? selected_slot() : -1;
    if (value == 0) {
        Statement row(
            "DELETE FROM unlocks WHERE character_slot=? AND bank=? AND slot=? AND lane=0");
        return row.write(owner, bank, slot);
    }
    Statement row("INSERT OR REPLACE INTO unlocks VALUES (?,?,?,0,?)");
    return row.write(owner, bank, slot, value);
}

/** Family-5 identity and the content gate are derived, not saved copies. */
bool read_family5(Family5State& output) noexcept {
    const std::lock_guard lock(g_mutex);
    output = {};
    output.objectSoid = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    Statement gates("SELECT count(*) FROM characters WHERE content_bypass=1");
    std::size_t bypassCount = 0;
    if (gates.step() != SQLITE_ROW || !gates.column(0, bypassCount)) {
        return false;
    }
    output.contentGateArm = bypassCount != 0;
    Statement rows("SELECT kind,position,slot,value FROM family5 ORDER BY kind,position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        int kind = 0;
        std::size_t position = 0;
        std::uint16_t slot = 0;
        std::int32_t value = 0;
        if (!rows.columns(kind, position, slot, value) || position >= kUnlockOverrideCapacity) {
            return false;
        }
        if (kind == 0 && position == output.flagCount && value >= 0 && value <= 255) {
            output.flags[output.flagCount++] = {slot, static_cast<std::uint8_t>(value)};
        } else if (kind == 1 && position == output.valueCount) {
            output.values[output.valueCount++] = {slot, value};
        } else {
            return false;
        }
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Saves both override lists in the same transaction. */
bool write_family5(const Family5State& value) noexcept {
    if (value.flagCount > value.flags.size() || value.valueCount > value.values.size()) {
        return false;
    }
    Transaction transaction;
    if (!transaction.ready() || !execute("DELETE FROM family5")) {
        return false;
    }
    Statement rows("INSERT INTO family5 VALUES (?,?,?,?)");
    for (std::size_t index = 0; index < value.flagCount; ++index) {
        if (!rows.write(0, index, value.flags[index].slot, value.flags[index].value)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < value.valueCount; ++index) {
        if (!rows.write(1, index, value.values[index].slot, value.values[index].value)) {
            return false;
        }
    }
    if (!transaction.commit()) {
        return false;
    }
    account::profiles::local_changed();
    return true;
}

/** Ownership rows use stable positions because their manifest handles depend on order. */
bool read_entitlements(entitlements::Table& output) noexcept {
    const std::lock_guard lock(g_mutex);
    output = {};
    Statement rows("SELECT position,name,ownership FROM entitlements ORDER BY position");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        std::size_t position = 0;
        entitlements::Ownership ownership{};
        std::string_view name;
        if (!rows.column(0, position) || !rows.text(1, name) || !rows.column(2, ownership)
            || position != output.count || position >= output.entries.size() || name.empty()
            || name.size() >= entitlements::kNameCapacity) {
            return false;
        }
        auto& entry = output.entries[output.count++];
        name.copy(entry.name.data(), name.size());
        entry.nameLength = static_cast<std::uint8_t>(name.size());
        entry.ownership = ownership;
        result = rows.step();
    }
    return result == SQLITE_DONE;
}

/** Completed bootstrap transforms must not overwrite later player progress on a restart. */
bool bootstrap_completed(std::string_view name) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement rows("SELECT name,completed FROM bootstrap");
    while (rows.step() == SQLITE_ROW) {
        std::string_view key;
        bool completed = false;
        if (rows.text(0, key) && rows.column(1, completed) && key == name) {
            return completed;
        }
    }
    return false;
}

/** The caller commits this marker together with the bootstrap's changes. */
bool complete_bootstrap(std::string_view name) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("INSERT OR REPLACE INTO bootstrap VALUES (?,1)");
    return row.write(name);
}

} // namespace sunrise::state::investment::store
