#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "../account/inventory/inventory_state.h"
#include "../build_data/pursuits/definition.h"

namespace sunrise::state::reward_resolution {
struct Context;
}

namespace sunrise::state {

/** One declared objective, evaluated with its installed comparison. */
struct PursuitObjectiveStatus {
    std::uint32_t hash{};
    /** The progress and target were read from the saved banks. */
    bool known{};
    bool complete{};
    /** An optional objective of a contract that requires only its other objectives. */
    bool skipped{};
    /** Progress is one of the held item's per-item values rather than a saved bank row. */
    bool itemHeld{};
    std::int32_t value{};
    std::int32_t target{};
};

/** A held pursuit's objectives evaluated against the saved banks. It does not permit a claim. */
struct PursuitCompletionStatus {
    /** The objective aggregate is determinate. */
    bool objectivesKnown{};
    /** In any-objective mode, one complete objective proves this even when others are unknown. */
    bool objectivesComplete{};
    std::uint8_t objectiveCount{};
    std::array<PursuitObjectiveStatus, build_data::pursuits::kObjectiveCapacity> objectives{};
};

/**
 * Evaluates every declared objective, then the aggregate under the contract's all-or-any mode,
 * without writing or drawing. The caller matches the contract's item index and hash to the held
 * instance.
 * @param definition Installed pursuit contract.
 * @param context Saved banks and class of the character holding the pursuit.
 * @param itemValues The held instance's per-item values in record order.
 * @param heldItems The character's held items, which an objective reading its slot's largest value
 *        across held instances also reads.
 */
[[nodiscard]] PursuitCompletionStatus
inspect_pursuit_completion(const build_data::pursuits::Definition& definition,
                           const reward_resolution::Context& context,
                           std::span<const std::int32_t> itemValues,
                           std::span<const account::inventory::Item> heldItems) noexcept;

} // namespace sunrise::state
