#pragma once

#include <algorithm>

#include "../../../core/network_capacity.h"
#include "definition.h"

namespace sunrise::state::activity::entity_slots {

/** Disjoint native entity masks for every member the group host already advertises. */
inline constexpr std::size_t kMemberLeaseBlockCount = core::network_capacity::kActivityPlayers;
/** Storage capacity does not override the activity's own native admission policy. */
inline constexpr std::size_t kMemberLeaseRowCount = kMemberLeaseBlockCount;
inline constexpr std::uint8_t kNoMemberLeaseBlock = 0xFF;

/** Per-member ownership, separate from the aggregate mask used by the simulation service. */
struct MemberLeases final {
    std::array<LeaseMask, kMemberLeaseRowCount> held{};
    std::array<std::uint8_t, kMemberLeaseRowCount> blocks{};
    /** Departed slots each surviving native recipient still needs to purge. */
    std::array<LeaseMask, kMemberLeaseRowCount> purgeOwed{};
    /** Held until a membership acknowledgement follows that recipient's purge. */
    std::array<LeaseMask, kMemberLeaseRowCount> retired{};
    std::array<std::uint32_t, kMemberLeaseRowCount> purgeRevision{};
    std::uint32_t joinedRows{};
    std::uint16_t blockWidth{};
    std::uint8_t nextBlock{};
    constexpr MemberLeases() noexcept {
        blocks.fill(kNoMemberLeaseBlock);
    }
};

struct MemberLeasePlan final {
    LeaseMask mask{};
    std::uint16_t blockWidth{};
    std::uint8_t block{kNoMemberLeaseBlock};
    bool fresh{};
    bool valid{};
    bool operator==(const MemberLeasePlan&) const noexcept = default;
};

[[nodiscard]] inline LeaseMask member_lease_block(std::size_t block, std::size_t width) noexcept {
    LeaseMask mask{};
    if (block >= kMemberLeaseBlockCount || width == 0
        || width > kSlotCount / kMemberLeaseBlockCount)
        return mask;
    for (std::size_t slot = block * width; slot < (block + 1) * width; ++slot)
        mask[slot / kSlotsPerByte] |=
            std::byte{static_cast<unsigned char>(1U << (slot % kSlotsPerByte))};
    return mask;
}

[[nodiscard]] inline bool
member_block_in_use(const MemberLeases& leases, std::size_t block, std::size_t exceptRow) noexcept {
    for (std::size_t row = 0; row < leases.blocks.size(); ++row)
        if (row != exceptRow && (leases.joinedRows & (1U << row)) && leases.blocks[row] == block)
            return true;
    const auto mask = member_lease_block(block, leases.blockWidth);
    for (const auto& retired : leases.retired)
        for (std::size_t index = 0; index < mask.size(); ++index)
            if ((mask[index] & retired[index]) != std::byte{}) return true;
    return false;
}

/** Pure plan, re-derived against the same native record before commit. */
[[nodiscard]] inline MemberLeasePlan plan_member_lease(const MemberLeases& leases,
                                                       std::size_t row,
                                                       const LeaseMask& serverReserve,
                                                       std::size_t requested) noexcept {
    MemberLeasePlan plan{};
    if (row >= kMemberLeaseRowCount || !requested) return plan;
    const auto capacity = (kSlotCount - slot_count(serverReserve)) / kMemberLeaseBlockCount;
    const auto width = leases.blockWidth ? leases.blockWidth : (std::min)(requested, capacity);
    if (!width || width > capacity || requested < width) return plan;
    std::size_t block = kNoMemberLeaseBlock;
    if (leases.joinedRows & (1U << row)) {
        block = leases.blocks[row];
    } else if (leases.nextBlock < kMemberLeaseBlockCount) {
        block = leases.nextBlock;
        plan.fresh = true;
    } else {
        // After a native departure, prefer that row's last block. Never alias another live row.
        const auto previous = leases.blocks[row];
        if (previous < kMemberLeaseBlockCount && !member_block_in_use(leases, previous, row))
            block = previous;
        else
            for (std::size_t candidate = 0; candidate < kMemberLeaseBlockCount; ++candidate)
                if (!member_block_in_use(leases, candidate, row)) {
                    block = candidate;
                    break;
                }
    }
    if (block >= kMemberLeaseBlockCount || member_block_in_use(leases, block, row)) return {};
    plan.mask = member_lease_block(block, width);
    for (std::size_t i = 0; i < plan.mask.size(); ++i)
        if ((plan.mask[i] & serverReserve[i]) != std::byte{}) return {};
    plan.blockWidth = static_cast<std::uint16_t>(width);
    plan.block = static_cast<std::uint8_t>(block);
    plan.valid = true;
    return plan;
}

/** The State transaction validates the complete plan before assigning it. */
inline void
assign_member_lease(MemberLeases& leases, std::size_t row, const MemberLeasePlan& plan) noexcept {
    if (row >= kMemberLeaseRowCount || !plan.valid || plan.block >= kMemberLeaseBlockCount) return;
    leases.held[row] = plan.mask;
    leases.blocks[row] = plan.block;
    leases.blockWidth = plan.blockWidth;
    leases.joinedRows |= 1U << row;
    if (plan.fresh) leases.nextBlock = static_cast<std::uint8_t>(plan.block + 1);
}

/** Native member departure releases this member alone and preserves the record's high-water mark.
 */
inline void depart_member_lease(MemberLeases& leases, std::size_t row) noexcept {
    if (row >= kMemberLeaseRowCount || !(leases.joinedRows & (1U << row))) return;
    for (std::size_t survivor = 0; survivor < kMemberLeaseRowCount; ++survivor) {
        if (survivor == row || !(leases.joinedRows & (1U << survivor))) continue;
        for (std::size_t index = 0; index < leases.held[row].size(); ++index) {
            leases.purgeOwed[survivor][index] |= leases.held[row][index];
            leases.retired[survivor][index] |= leases.held[row][index];
        }
        leases.purgeRevision[survivor] = 0;
    }
    leases.purgeOwed[row] = {};
    leases.retired[row] = {};
    leases.purgeRevision[row] = 0;
    leases.held[row] = {};
    leases.joinedRows &= ~(1U << row);
}

/** A later native membership receipt proves the earlier purge crossed the ordered BAP stream. */
inline void
acknowledge_member_purge(MemberLeases& leases, std::size_t row, std::uint32_t revision) noexcept {
    if (row >= kMemberLeaseRowCount || !leases.purgeRevision[row]
        || revision < leases.purgeRevision[row] || slot_count(leases.purgeOwed[row]))
        return;
    leases.retired[row] = {};
    leases.purgeRevision[row] = 0;
}

[[nodiscard]] inline LeaseMask aggregate_member_leases(const MemberLeases& leases) noexcept {
    LeaseMask result{};
    for (const auto& held : leases.held)
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] |= held[i];
    return result;
}
} // namespace sunrise::state::activity::entity_slots
