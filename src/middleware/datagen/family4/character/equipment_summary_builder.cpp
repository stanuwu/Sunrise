#include "equipment_summary_builder.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sunrise::middleware::datagen::family4::character {
namespace {

namespace light = state::equipment::light;

/** Every native equipment slot can carry a powered item, so all 20 may be counted. */
constexpr std::int32_t kMaximumDivisor =
    static_cast<std::int32_t>(state::build_data::items::details::kEquipmentSlotCount);

/**
 * Validates one semantic summary array without relying on sentinel values.
 * An unpowered item is present with score 0, so only a negative score is invalid.
 * @param scores Profile or selected-character native slot scores.
 * @return True when every present definition index and score is wire-safe.
 */
[[nodiscard]] bool valid_scores(const light::SlotScores& scores) noexcept {
    for (const std::optional<light::ItemScore>& score : scores) {
        if (score.has_value()
            && (score->definitionIndex == layout::kEmptySummaryDefinitionIndex
                || score->score < 0)) {
            return false;
        }
    }
    return true;
}

/**
 * Maps optional semantic score rows into explicit native sentinel records.
 * @param scores Validated semantic native-slot scores.
 * @param output Destination native summary array.
 */
void map_scores(const light::SlotScores& scores,
                std::span<layout::EquipmentSummaryEntry> output) noexcept {
    for (std::size_t slot = 0; slot < scores.size(); ++slot) {
        layout::EquipmentSummaryEntry& entry = output[slot];
        entry.definitionIndex = layout::kEmptySummaryDefinitionIndex;
        if (scores[slot].has_value()) {
            entry.definitionIndex = scores[slot]->definitionIndex;
            entry.score = scores[slot]->score;
        }
    }
}

} // namespace

/** Maps a complete semantic light evaluation into the wire ABI. */
bool build_equipment_summary(const light::Evaluation& evaluation,
                             layout::EquipmentSummary& output) noexcept {
    if (!valid_scores(evaluation.profile) || !valid_scores(evaluation.character)
        || evaluation.divisor <= 0 || evaluation.divisor > kMaximumDivisor || evaluation.total < 0
        || evaluation.average != evaluation.total / evaluation.divisor
        || !std::isfinite(evaluation.averageFloat)
        || evaluation.averageFloat
               != static_cast<float>(evaluation.total) / static_cast<float>(evaluation.divisor)) {
        return false;
    }

    layout::EquipmentSummary staged{};
    map_scores(evaluation.profile, staged.profile);
    map_scores(evaluation.character, staged.character);
    staged.divisor = evaluation.divisor;
    staged.total = evaluation.total;
    staged.light = evaluation.average;
    staged.lightScalar = evaluation.averageFloat;
    output = staged;
    return true;
}

} // namespace sunrise::middleware::datagen::family4::character
