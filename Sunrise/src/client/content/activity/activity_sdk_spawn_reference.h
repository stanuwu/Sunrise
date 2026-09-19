#pragma once

#include <cstdint>

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {

enum class SpawnReferenceState : std::uint8_t {
    invalid,
    absent,
    present,
};

/** Build86657 native unset tests are typeFF or indexFFFF; malformed is not absent. */
[[nodiscard]] constexpr SpawnReferenceState spawn_reference_state(std::uint64_t raw) noexcept {
    const auto key = static_cast<std::uint32_t>(raw);
    const auto type = static_cast<std::uint8_t>(raw >> 32U);
    const auto padding = static_cast<std::uint8_t>(raw >> 40U);
    const auto index = static_cast<std::uint16_t>(raw >> 48U);
    if (padding != 0) {
        return SpawnReferenceState::invalid;
    }
    if (type == 0xFFU || index == 0xFFFFU) {
        return SpawnReferenceState::absent;
    }
    return key != 0 && key != 0xFFFFFFFFU && type >= 1 && type <= 72 ? SpawnReferenceState::present
                                                                     : SpawnReferenceState::invalid;
}

/** Source proof only: the selected rule and its occurrence still need independent validation. */
[[nodiscard]] constexpr bool requires_selected_spawn_rule(std::uint64_t reference98,
                                                          std::uint64_t referenceA0,
                                                          bool complete,
                                                          bool inlinePointSetInspected,
                                                          bool hasInlinePointSet) noexcept {
    return complete && inlinePointSetInspected && !hasInlinePointSet
           && spawn_reference_state(reference98) == SpawnReferenceState::absent
           && spawn_reference_state(referenceA0) == SpawnReferenceState::absent;
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
