#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::vendors {

/** Bounds the archived build's faction token values and their matching placeholders. */
inline constexpr std::size_t kReputationTurnInCapacity = 32;

/** One build-matched relationship omitted from the installed faction rows. */
struct ReputationTurnIn {
    std::uint32_t factionHash{};
    std::uint32_t placeholderHash{};
    std::uint32_t costHash{};
    std::int32_t experiencePerUnit{};
};

/** Loads the build-scoped faction turn-in relationships embedded in the DLL. */
[[nodiscard]] bool load_reputation_turn_ins(void* module) noexcept;

/** Withdraws every faction turn-in relationship before shutdown or failed publication. */
void clear_reputation_turn_ins() noexcept;

/**
 * Finds one faction's authored token value for an installed placeholder and cost item.
 * @param factionHash Faction resolved from the installed vendor definition.
 * @param placeholderHash Item sold by the installed sale row.
 * @param costHash Item charged by the installed sale row.
 * @param turnIn Receives the matching relationship.
 * @return True when the exact build-scoped relationship exists.
 */
[[nodiscard]] bool find_reputation_turn_in(std::uint32_t factionHash,
                                           std::uint32_t placeholderHash,
                                           std::uint32_t costHash,
                                           ReputationTurnIn& turnIn) noexcept;

/** @return True when any retained faction turn-in uses the item as its placeholder. */
[[nodiscard]] bool is_reputation_placeholder(std::uint32_t placeholderHash) noexcept;

} // namespace sunrise::state::build_data::vendors
