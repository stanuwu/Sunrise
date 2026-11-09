#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "inventory_state.h"

namespace sunrise::state::account::inventory {

// --- Authored game data --------------------------------------------------------------------

// TODO: extract the Synthesizer tier order; no extractor relates one Synthesizer definition to the
// tier after it.

/** Weak, Middling and Powerful Gambit Prime Synthesizers; a definition's tier is its position. */
inline constexpr std::array<std::uint32_t, 3> kSynthesizerTiers{
    1160544509U, // Weak
    1160544508U, // Middling
    1160544511U, // Powerful
};

/**
 * The unlock value the client reads a character's Synthesizer tier from. Its row in each
 * character's value block of the account object is read from the value maps.
 */
inline constexpr std::uint32_t kSynthesizerTierValueHash = 2279449926U;

/** The tier of anything that is not a Synthesizer, and of a character holding none. */
inline constexpr std::int32_t kNoSynthesizerTier = 0;

/** @return The Synthesizer tier of a definition, from 1 for Weak, or kNoSynthesizerTier. */
[[nodiscard]] inline std::int32_t synthesizer_tier(std::uint32_t definitionHash) noexcept {
    const auto found =
        std::find(kSynthesizerTiers.begin(), kSynthesizerTiers.end(), definitionHash);
    return found == kSynthesizerTiers.end()
               ? kNoSynthesizerTier
               : static_cast<std::int32_t>(found - kSynthesizerTiers.begin()) + 1;
}

/**
 * Finds the one Synthesizer a character holds. Synthesizers have no equipment slot.
 * @return Its inventory index, or the inventory count when none or more than one is held.
 */
[[nodiscard]] inline std::size_t held_synthesizer(const CharacterItems& items) noexcept {
    const auto held = std::span(items.values).first(items.count);
    std::size_t found = items.count;
    for (std::size_t index = 0; index < held.size(); ++index) {
        if (synthesizer_tier(held[index].definitionHash) == kNoSynthesizerTier) {
            continue;
        }
        if (found != items.count) {
            return items.count;
        }
        found = index;
    }
    return found;
}

} // namespace sunrise::state::account::inventory
