#pragma once

#include <cstdint>

namespace sunrise::server::activity::mission::observation_epoch {

/** Reported Sense object identity. This is not an individual actor generation or death receipt. */
struct Cursor final {
    std::uint64_t sourceGeneration{};
    std::uint64_t sequence{};
    std::uint32_t generationPlusOne{};
    bool hasGeneration{};
    bool used{};
};

enum class Transition : std::uint8_t { discard, baseline, continuous };

/**
 * Counts from different reported object generations cannot form a gameplay edge. Missing
 * generations also require reconciliation, following Host's same_sense_scalar_generations rule.
 * The caller publishes the baseline as state, without inventing a spawn or death.
 */
[[nodiscard]] inline Transition accept(Cursor& retained,
                                       std::uint64_t expectedSourceGeneration,
                                       std::uint64_t sourceGeneration,
                                       std::uint64_t sequence,
                                       std::uint32_t generationPlusOne,
                                       bool hasGeneration) noexcept {
    if (sourceGeneration == 0 || sourceGeneration != expectedSourceGeneration || sequence == 0
        || (retained.used && retained.sourceGeneration == sourceGeneration
            && sequence <= retained.sequence)) {
        return Transition::discard;
    }
    const bool continuous = retained.used && retained.sourceGeneration == sourceGeneration
                            && retained.hasGeneration && hasGeneration
                            && retained.generationPlusOne == generationPlusOne;
    retained = {sourceGeneration, sequence, generationPlusOne, hasGeneration, true};
    return continuous ? Transition::continuous : Transition::baseline;
}

} // namespace sunrise::server::activity::mission::observation_epoch
