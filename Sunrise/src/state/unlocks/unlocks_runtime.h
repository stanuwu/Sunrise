#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::unlocks {

/**
 * Seeds the live unlock banks from the boot policy.
 * @param table Complete authored policy.
 */
void publish(const Table& table) noexcept;

/** @return The live unlock banks. Every writer runs on the server thread. */
[[nodiscard]] Table get() noexcept;
[[nodiscard]] bool snapshot(Table& output, int characterSlot = -1) noexcept;

/** Restores empty unlock banks. */
void clear() noexcept;

/**
 * Runs one write over the live banks under the exclusive lock.
 * The callback must not re-enter this module.
 * @param context Passed through untouched.
 * @param apply Called once with the live banks.
 */
bool mutate(void* context, void (*apply)(void*, Table&) noexcept) noexcept;

/**
 * Runs one write over the live banks under the exclusive lock, saving them only when it reports
 * a change. The callback must not re-enter this module.
 * @param context Passed through untouched.
 * @param apply Called once with the live banks; returns whether it changed them.
 * @return False when the banks do not read or a change does not save.
 */
bool mutate_if_changed(void* context, bool (*apply)(void*, Table&) noexcept) noexcept;

/** @param index Account flag bank row. @return True when the flag is set. */
[[nodiscard]] bool account_flag_set(std::uint16_t index) noexcept;

/** @param index Character object flag bank row. @return True when the flag is set. */
[[nodiscard]] bool character_object_flag_set(std::uint16_t index) noexcept;

/**
 * Writes one account acquired flag.
 * @param index Account flag bank row.
 * @param value Biased flag value.
 * @return False when the row is outside the bank.
 */
bool set_account_flag(std::uint16_t index, std::uint8_t value) noexcept;

/** @param index Account value bank row. @return The value, or zero outside the bank. */
[[nodiscard]] std::int32_t objective_value(std::uint16_t index) noexcept;

/**
 * Writes one selected-character object value.
 * @param index Character object value bank row.
 * @param value Replicated value.
 * @return False when the row is outside the bank.
 */
bool set_character_object_value(std::uint16_t index, std::int32_t value) noexcept;

/** @param definitionIndex Native progression row. @return Lane 0, or zero outside the bank. */
[[nodiscard]] std::int32_t account_progression(std::uint16_t definitionIndex) noexcept;

/**
 * Writes lane 0 of one account progression.
 * @param definitionIndex Native progression row.
 * @param value Progress the level walk reads.
 * @return False when the row is outside the bank.
 */
bool set_account_progression(std::uint16_t definitionIndex, std::int32_t value) noexcept;

} // namespace sunrise::state::unlocks
