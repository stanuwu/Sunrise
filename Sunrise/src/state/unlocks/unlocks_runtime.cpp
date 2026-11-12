#include "unlocks_runtime.h"

#include "../investment/store_internal.h"

namespace sunrise::state::unlocks {
namespace store = investment::store;

/** Replaces the saved unlock banks in one transaction. */
void publish(const Table& table) noexcept {
    (void)store::write_unlocks(table);
}

/** @return A call-local copy of the saved banks. */
Table get() noexcept {
    Table table;
    if (!store::read_unlocks(table)) {
        table = {};
    }
    return table;
}

/** Reads the requested character's banks and the shared account banks together. */
bool snapshot(Table& output, int characterSlot) noexcept {
    return store::read_unlocks(output, characterSlot);
}

/** Clears the saved banks for the active account and selected character. */
void clear() noexcept {
    (void)store::write_unlocks(Table{});
}

/** Applies one mutation and reports success only after its database commit. */
bool mutate(void* context, void (*apply)(void*, Table&) noexcept) noexcept {
    if (apply == nullptr) {
        return false;
    }
    store::Transaction transaction;
    Table table;
    if (!transaction.ready() || !store::read_unlocks(table)) {
        return false;
    }
    apply(context, table);
    return store::write_unlocks(table) && transaction.commit();
}

/** An unchanged pass skips the save, which reads every bank again to find the changed rows. */
bool mutate_if_changed(void* context, bool (*apply)(void*, Table&) noexcept) noexcept {
    if (apply == nullptr) {
        return false;
    }
    store::Transaction transaction;
    Table table;
    if (!transaction.ready() || !store::read_unlocks(table)) {
        return false;
    }
    return (!apply(context, table) || store::write_unlocks(table)) && transaction.commit();
}

/** Reads one saved accountFlags entry. */
bool account_flag_set(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded =
        index < kAccountFlagCapacity && store::read_unlock(store::Bank::accountFlags, index, value);
    return loaded && value == kFlagSet;
}

/** Reads one saved characterObjectFlags entry. */
bool character_object_flag_set(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded = index < kCharacterObjectFlagCapacity
                        && store::read_unlock(store::Bank::characterObjectFlags, index, value);
    return loaded && value == kFlagSet;
}

/** Reads one saved objectiveValues entry. */
std::int32_t objective_value(std::uint16_t index) noexcept {
    std::int32_t value = 0;
    const bool loaded = index < kObjectiveValueCapacity
                        && store::read_unlock(store::Bank::objectiveValues, index, value);
    return loaded ? value : 0;
}

/** Reads one saved accountProgressions entry. */
std::int32_t account_progression(std::uint16_t definitionIndex) noexcept {
    std::int32_t value = 0;
    const bool loaded =
        definitionIndex < build_data::progressions::kDefinitionCapacity
        && store::read_unlock(store::Bank::accountProgressions, definitionIndex, value);
    return loaded ? value : 0;
}

/** Saves one bounded accountFlags entry. */
bool set_account_flag(std::uint16_t index, std::uint8_t value) noexcept {
    return index < kAccountFlagCapacity
           && store::write_unlock(store::Bank::accountFlags, index, value);
}

/** Saves one bounded characterObjectValues entry. */
bool set_character_object_value(std::uint16_t index, std::int32_t value) noexcept {
    return index < kCharacterObjectValueCapacity
           && store::write_unlock(store::Bank::characterObjectValues, index, value);
}

/** Saves one bounded accountProgressions entry. */
bool set_account_progression(std::uint16_t definitionIndex, std::int32_t value) noexcept {
    return definitionIndex < build_data::progressions::kDefinitionCapacity
           && store::write_unlock(store::Bank::accountProgressions, definitionIndex, value);
}

} // namespace sunrise::state::unlocks
