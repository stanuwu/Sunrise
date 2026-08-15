#include "equipment_light_calculation.h"

#include <limits>

namespace sunrise::state::equipment::light::calculation {
namespace {

/**
 * Applies strict score upgrades from one profile or other-character source.
 * @param selectedCharacter Selected scores that define which slots may be upgraded.
 * @param candidate Candidate scores from one independent ownership source.
 * @param aggregate Selected-seeded values updated in place.
 */
void merge_strict_upgrades(const SlotScores& selectedCharacter,
                           const SlotScores& candidate,
                           SlotScores& aggregate) noexcept {
    for (std::size_t slotIndex = 0; slotIndex < aggregate.size(); ++slotIndex) {
        // Candidates cannot create a slot absent from the selected character.
        if (!selectedCharacter[slotIndex].has_value() || !candidate[slotIndex].has_value()) {
            continue;
        }

        // A strict compare keeps the selected item winning when scores tie.
        if (candidate[slotIndex]->score > aggregate[slotIndex]->score) {
            aggregate[slotIndex] = candidate[slotIndex];
        }
    }
}

/**
 * Builds the private score set used only for the weighted aggregate.
 * @param selectedCharacter Selected scores that define eligible slots and tie ownership.
 * @param profileSlotMaxima Profile-owned candidates applied before other characters.
 * @param otherCharacterScores Other-character candidates in stable account order.
 * @return Selected-seeded scores with every strict upgrade applied.
 */
[[nodiscard]] SlotScores
merge_aggregate(const SlotScores& selectedCharacter,
                const SlotScores& profileSlotMaxima,
                const std::span<const SlotScores> otherCharacterScores) noexcept {
    SlotScores aggregate = selectedCharacter;
    merge_strict_upgrades(selectedCharacter, profileSlotMaxima, aggregate);
    for (const SlotScores& otherCharacter : otherCharacterScores) {
        merge_strict_upgrades(selectedCharacter, otherCharacter, aggregate);
    }
    return aggregate;
}

/** Adds one present score. An absent slot must add zero. */
void add_slot_score(const SlotScores& scores,
                    const std::size_t slotIndex,
                    std::int64_t& total) noexcept {
    if (scores[slotIndex].has_value()) {
        total += scores[slotIndex]->score;
    }
}

/**
 * Sums every slot score in 64-bit storage.
 * The native producer totals all 20 slots, so an unpowered slot adds its zero rather than being
 * left out.
 * @param aggregate Selected-compatible maximum scores.
 * @return Signed wide total before the summary-field range check.
 */
[[nodiscard]] std::int64_t slot_total(const SlotScores& aggregate) noexcept {
    std::int64_t total = 0;
    for (std::size_t slotIndex = 0; slotIndex < aggregate.size(); ++slotIndex) {
        add_slot_score(aggregate, slotIndex, total);
    }
    return total;
}

/** @param value Signed wide total. @return True when the native summary field can store it. */
[[nodiscard]] bool fits_summary_integer(const std::int64_t value) noexcept {
    return value >= std::numeric_limits<std::int32_t>::min()
           && value <= std::numeric_limits<std::int32_t>::max();
}

/** @param aggregate Merged slot scores. @return How many slots carry a positive score. */
[[nodiscard]] std::int32_t divisor_for(const SlotScores& aggregate) noexcept {
    std::int32_t counted = 0;
    for (const std::optional<ItemScore>& score : aggregate) {
        if (score.has_value() && score->score > kUnpoweredScore) {
            ++counted;
        }
    }
    return counted;
}

} // namespace

/** Computes raw summary arrays and the selected character's merged weighted light values. */
bool evaluate(const SlotScores& selectedCharacter,
              const SlotScores& profileSlotMaxima,
              std::span<const SlotScores> otherCharacterScores,
              Evaluation& output) noexcept {
    const SlotScores aggregate =
        merge_aggregate(selectedCharacter, profileSlotMaxima, otherCharacterScores);
    const std::int64_t wideTotal = slot_total(aggregate);
    if (!fits_summary_integer(wideTotal)) {
        return false;
    }

    Evaluation candidate;
    candidate.profile = profileSlotMaxima;
    candidate.character = selectedCharacter;
    candidate.divisor = divisor_for(aggregate);
    candidate.total = static_cast<std::int32_t>(wideTotal);
    // A character wearing nothing powered has no average to take, and dividing would trap.
    candidate.average = candidate.divisor == 0 ? 0 : candidate.total / candidate.divisor;
    candidate.averageFloat = candidate.divisor == 0 ? 0.0F
                                                    : static_cast<float>(candidate.total)
                                                          / static_cast<float>(candidate.divisor);
    output = candidate;
    return true;
}

} // namespace sunrise::state::equipment::light::calculation
