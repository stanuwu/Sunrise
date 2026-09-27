#pragma once

#include <cstdint>

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {

/** How one raw 64-bit spawn reference (key32, type8, padding8, index16) reads. */
enum class SpawnReferenceState : std::uint8_t {
    invalid,
    absent,
    present,
};

/**
 * The client treats a reference as unset when its type is 0xFF or its index is 0xFFFF. Nonzero
 * padding or an out-of-range type is malformed, which is not the same as absent.
 */
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

/**
 * A spawner needs a selected rule when both of its rule references (at offsets 0x98 and 0xA0) are
 * absent and a complete parse found no inline point set. This proves only the source; the selected
 * rule and its occurrence are validated separately.
 */
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
