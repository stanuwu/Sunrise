#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../account/account_state.h"
#include "../build_data/inventory/buckets/definition.h"
#include "../build_data/runtime.h"

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

    /** Chooses an append or FIFO replacement; a full ordinary bucket refuses the insertion. */
    [[nodiscard]] bool select(const build_data::inventory::buckets::Descriptor& bucket,
                              std::size_t appendIndex,
                              std::size_t& index) const noexcept {
        index = appendIndex;
        if (count < bucket.slotCount) {
            return true;
        }
        if (count != bucket.slotCount
            || (bucket.policyFlags & build_data::inventory::buckets::kFifo) == 0
            || oldest >= appendIndex) {
            return false;
        }
        index = oldest;
        return true;
    }
};

/** Character rows share one FIFO order across instances and stacks. */
inline constexpr std::size_t kCharacterStackAdmissionBase =
    account::inventory::kCharacterItemCapacity;
inline constexpr std::size_t kCharacterAdmissionCapacity =
    kCharacterStackAdmissionBase + account::inventory::kCharacterStackCapacity;

/** Counts equipped rows but chooses eviction candidates only from held instances and stacks. */
[[nodiscard]] bool
character_bucket_admission(const CharacterState& character,
                           const build_data::inventory::buckets::Descriptor& bucket,
                           BucketAdmission& occupancy) noexcept;
/** Removes a selected combined-array row while keeping both saved arrays dense. */
[[nodiscard]] bool erase_character_bucket_row(CharacterState& character, std::size_t row) noexcept;
/** Resolves an instance Lost Items may hold, and the authored character bucket it returns to. */
[[nodiscard]] bool
lost_items_candidate(const account::inventory::Item& item,
                     build_data::items::Definition& definition,
                     build_data::inventory::buckets::Descriptor& bucket) noexcept;
/**
 * Applies installed FIFO policy, or redirects an eligible instance into Lost Items when its
 * authored bucket is full. Eviction updates the character copy before the caller appends the item.
 * @param before Account the capacity decision reads.
 * @param characterIndex Character receiving the reward.
 * @param item Reward whose placement is decided.
 * @param after Character copy receiving any eviction.
 * @param evictedInstanceSoid Receives the removed QueueZ resident, or zero for a stack.
 */
[[nodiscard]] bool place_instanced_reward(const AccountState& before,
                                          std::size_t characterIndex,
                                          account::inventory::Item& item,
                                          CharacterState& after,
                                          std::uint64_t& evictedInstanceSoid) noexcept;

} // namespace sunrise::state::runtime::detail
