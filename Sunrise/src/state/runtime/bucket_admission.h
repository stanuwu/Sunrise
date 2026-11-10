#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail {

/** One index space over a character's held instances, stacks and equipped rows, in that order. */
inline constexpr std::size_t kStackRowBase = account::inventory::kCharacterItemCapacity;
inline constexpr std::size_t kEquippedRowBase =
    kStackRowBase + account::inventory::kCharacterStackCapacity;
inline constexpr std::size_t kCharacterRowCount =
    kEquippedRowBase + account::inventory::kEquipmentSlotCount;

/** Rows one batch touched, keeping the newest each change ring can announce. */
struct PlacedRows {
    std::array<GrantedRow, kRecordRewardGrantCapacity> values{};
    std::size_t count{};
};

/** One item to place, and the instance it becomes when its definition is instanced. */
struct Arrival {
    build_data::items::Definition item{};
    build_data::items::details::Definition detail{};
    std::int32_t quantity{};
    /** Level and sockets of a new instance; placement assigns its identity and serial. */
    account::inventory::Item instance{};
};

/**
 * Finds the recovery bucket an item names when a saved row can occupy it. An arrival's overflow
 * lands there.
 * @return False when the item names no recovery bucket, or one outside the character array.
 */
[[nodiscard]] bool
find_recovery_bucket(const build_data::items::Definition& item,
                     build_data::inventory::buckets::Descriptor& recovery) noexcept;

/**
 * Places one arrival the way the client's grant does: what fits in the item's own bucket, then the
 * remainder in its recovery bucket, then nothing for the rest. A full FIFO bucket evicts its oldest
 * row, which moves to its own recovery bucket when the bucket allows a transfer.
 * @param working Account the arrival lands in; the selected character receives character rows.
 * @param characterIndex Selected character.
 * @param nextInstanceSoid Lowest identity a new instance may take; advanced past each one used.
 * @param rows Receives every row the arrival created, grew or moved.
 * @param placed Receives the quantity that found a row; the client drops the rest unrecorded.
 * @param reason Receives a static label when placement cannot proceed.
 * @return Unresolvable when the item's own data cannot be placed; deferred when the account
 *         refuses it now. A dropped remainder is still prepared.
 */
[[nodiscard]] RewardPreparation place_arrival(AccountState& working,
                                              std::size_t characterIndex,
                                              const Arrival& arrival,
                                              std::uint64_t& nextInstanceSoid,
                                              PlacedRows& rows,
                                              std::int32_t& placed,
                                              const char*& reason) noexcept;

/**
 * Moves one claim from the item's recovery bucket back to its authored bucket once the client's
 * claim validation passes: presence is the quantity all its rows there hold. The source is then
 * debited from its last row, and the destination merges, fills empty rows and evicts under its own
 * policy, in the order of the client's own helpers.
 * @param sourceBucket The bucket the claim names; it must be the item's recovery bucket.
 * @param instanceSoid The claimed instance, or zero for a stack.
 * @param reason Receives the static refusal label.
 * @return False when the client's transfer validation would refuse the claim.
 */
[[nodiscard]] bool place_claim(AccountState& working,
                               std::size_t characterIndex,
                               std::uint8_t sourceBucket,
                               const build_data::items::Definition& item,
                               const build_data::items::details::Definition& detail,
                               std::uint64_t instanceSoid,
                               std::int32_t quantity,
                               PlacedRows& rows,
                               const char*& reason) noexcept;

/**
 * Each native change list must fit independently. Only rows still present are announced; unlock
 * flags and dropped quantity publish no change record.
 * @return True when both lists hold the rows.
 */
[[nodiscard]] bool change_rings_fit(std::span<const GrantedRow> rows) noexcept;

/** @return True when a row with this serial is still in the after-image of its array. */
[[nodiscard]] bool row_present(const AccountState& account,
                               std::size_t characterIndex,
                               const GrantedRow& row) noexcept;

/**
 * Stores the selected character and profile rows a batch is placed against, so the batch needs
 * only one whole working account.
 */
void record_grant_before(const AccountState& before,
                         std::size_t characterIndex,
                         PendingRecordRewardGrant& mutation) noexcept;

/** Stores a placed batch's after-image and the rows still present in it. */
void record_grant_after(const AccountState& after,
                        const PlacedRows& placed,
                        PendingRecordRewardGrant& mutation) noexcept;

/**
 * Takes the character's next inventory serial, which a changed character row receives.
 * @return False when the character's serials are exhausted.
 */
[[nodiscard]] bool next_character_serial(CharacterState& character,
                                         std::int32_t& mutationSerial) noexcept;

/**
 * Picks the serial a changed profile row receives.
 * @return False when the profile's serials are exhausted.
 */
[[nodiscard]] bool next_profile_serial(const AccountState& account,
                                       std::int32_t& mutationSerial) noexcept;

/**
 * Removes one character row and closes the gap in its array.
 * @param row Index in the character row space that kStackRowBase and kEquippedRowBase divide.
 * @return False when no row is held there.
 */
[[nodiscard]] bool erase_character_row(CharacterState& character, std::size_t row) noexcept;

/**
 * Removes one profile row and closes the gap.
 * @return False when no row is held there.
 */
[[nodiscard]] bool erase_profile_row(AccountState& account, std::size_t index) noexcept;

} // namespace sunrise::state::runtime::detail
