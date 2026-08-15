#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../build_data/items/details/definition.h"

namespace sunrise::state::equipment::light {

/** One present item gives its definition identity and its runtime light score. */
struct ItemScore {
    std::uint16_t definitionIndex{};
    std::int32_t score{};

    [[nodiscard]] bool operator==(const ItemScore&) const noexcept = default;
};

/** An empty optional is the only absent-item marker in the 20 native equipment slots. */
using SlotScores =
    std::array<std::optional<ItemScore>, build_data::items::details::kEquipmentSlotCount>;

/** Item power comes from the item level, at 10 power per level. */
inline constexpr std::int32_t kPowerPerLevel = 10;
/** A powered item never scores below this floor whatever its level. */
inline constexpr std::int32_t kMinimumItemPower = 750;

/** A slot at or below this score adds nothing and is not counted by the divisor. */
inline constexpr std::int32_t kUnpoweredScore = 0;

/** The raw arrays and totals an equipment-light summary needs. */
struct Evaluation {
    SlotScores profile{};
    SlotScores character{};
    std::int32_t divisor{};
    std::int32_t total{};
    /** Signed integer division cuts the fraction toward zero. */
    std::int32_t average{};
    /** Float division keeps the fractional average, for display later. */
    float averageFloat{};

    [[nodiscard]] bool operator==(const Evaluation&) const noexcept = default;
};

/** One final scalar keeps the integer average it came from. */
struct ScalarValue {
    std::int32_t average{};
    float value{};

    [[nodiscard]] bool operator==(const ScalarValue&) const noexcept = default;
};

} // namespace sunrise::state::equipment::light
