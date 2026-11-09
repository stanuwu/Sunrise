#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../account/account_state.h"
#include "../build_data/inventory/buckets/definition.h"
#include "../build_data/runtime.h"
#include "runtime.h"

namespace sunrise::state::runtime::detail {

/** Counts occupied rows and remembers the oldest replaceable row in their owning array. */
struct BucketAdmission {
    std::size_t count{};
    std::size_t oldest{(std::numeric_limits<std::size_t>::max)()};
    std::int32_t serial{(std::numeric_limits<std::int32_t>::max)()};

    /** Counts one replaceable row and keeps the lowest mutation serial as the oldest. */
    void include(std::size_t index, std::int32_t mutationSerial) noexcept {
        ++count;
        if (oldest == (std::numeric_limits<std::size_t>::max)() || mutationSerial < serial) {
            oldest = index;
            serial = mutationSerial;
        }
    }
};

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

/** How much of one arrival found a row; the client drops the rest without a record. */
struct ArrivalOutcome {
    std::int32_t placed{};
    std::int32_t dropped{};
};

/**
 * Places one arrival the way the client's grant does: what fits in the item's own bucket, then the
 * remainder in its recovery bucket, then nothing for the rest. A full FIFO bucket evicts its oldest
 * row, which moves to its own recovery bucket when the bucket allows a transfer.
 * @param working Account the arrival lands in; the selected character receives character rows.
 * @param characterIndex Selected character.
 * @param nextInstanceSoid Lowest identity a new instance may take; advanced past each one used.
 * @param rows Receives every row the arrival created, grew or moved.
 * @param outcome Receives the placed and dropped quantities.
 * @param reason Receives a static label when placement cannot proceed.
 * @return Unresolvable when the item's own data cannot be placed; deferred when the account
 *         refuses it now. A dropped remainder is still prepared.
 */
[[nodiscard]] RewardPreparation place_arrival(AccountState& working,
                                              std::size_t characterIndex,
                                              const Arrival& arrival,
                                              std::uint64_t& nextInstanceSoid,
                                              PlacedRows& rows,
                                              ArrivalOutcome& outcome,
                                              const char*& reason) noexcept;

/**
 * Moves one claim from the item's recovery bucket back to its authored bucket once the client's
 * claim validation passes: presence is the quantity all its rows there hold. The source is then
 * debited from its last row, and the destination merges, fills empty rows and evicts under its own
 * policy, in the order of the client's own helpers; the retail server's executor is unobserved.
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
 * @return Null when both lists fit, else the static label of the one that does not.
 */
[[nodiscard]] const char* change_ring_refusal(std::span<const GrantedRow> rows) noexcept;

/** @return True when a row with this serial is still in the after-image of its array. */
[[nodiscard]] bool row_present(const AccountState& account,
                               std::size_t characterIndex,
                               const GrantedRow& row) noexcept;

/** Stores a placed batch's before and after images and the rows still present after it. */
void record_grant_images(const AccountState& before,
                         const AccountState& after,
                         std::size_t characterIndex,
                         const PlacedRows& placed,
                         PendingRecordRewardGrant& mutation) noexcept;

} // namespace sunrise::state::runtime::detail
