#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Type-2 Sense carries two normalized runtime damage pools. */
inline constexpr std::uint32_t kCombatantDamageSenseSchema = 0x80807DA2U;
inline constexpr std::uint8_t kCombatantDamageSlotType = 2;
/** Root ordinals of the accepted spawn revision, pools and lifecycle flags. */
inline constexpr std::uint16_t kCombatantDamageRevisionOrdinal = 0;
inline constexpr std::uint16_t kCombatantPrimaryDamageOrdinal = 8;
inline constexpr std::uint16_t kCombatantSecondaryDamageOrdinal = 9;
inline constexpr std::uint16_t kCombatantSuppressedOrdinal = 10;
inline constexpr std::uint16_t kCombatantFirstAuthOrdinal = 11;

/** Observed fractions do not prove that the combatant has an actor. */
struct CombatantDamageLevel final {
    std::int32_t revision{};
    float primary{-1.0F};
    float secondary{-1.0F};
    bool suppressed{};
    bool firstAuthApplied{};
    bool observed{};
    bool operator==(const CombatantDamageLevel&) const = default;
};

/**
 * Merges optional pool fields while clearing fractions across combatant lifetimes.
 * @param retained Last accepted level for this exact combatant and client generation.
 * @param values Accepted Type-2 Sense fields.
 * @param root Native root schema identity.
 * @return True when the observed level changed; invalid input leaves it unchanged.
 */
[[nodiscard]] inline bool update_combatant_damage(
    CombatantDamageLevel& retained,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    CombatantDamageLevel next = retained;
    float primary = -1.0F;
    float secondary = -1.0F;
    bool relevant = false;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != root || value.occurrence != 0) {
            continue;
        }
        switch (value.fieldOrdinal) {
        case kCombatantDamageRevisionOrdinal:
            if (value.kind != sense::ValueKind::signedInteger
                || value.signedValue < retained.revision
                || value.signedValue > (std::numeric_limits<std::int32_t>::max)()) {
                return false;
            }
            next.revision = static_cast<std::int32_t>(value.signedValue);
            relevant = true;
            break;
        case kCombatantPrimaryDamageOrdinal:
        case kCombatantSecondaryDamageOrdinal:
            if (value.kind != sense::ValueKind::real32 || !std::isfinite(value.realValue)
                || value.realValue < 0.0F || value.realValue > 1.0F) {
                return false;
            }
            if (value.fieldOrdinal == kCombatantPrimaryDamageOrdinal) {
                primary = value.realValue;
            } else {
                secondary = value.realValue;
            }
            relevant = true;
            break;
        case kCombatantSuppressedOrdinal:
        case kCombatantFirstAuthOrdinal:
            if (value.kind != sense::ValueKind::boolean || value.unsignedValue > 1) {
                return false;
            }
            if (value.fieldOrdinal == kCombatantSuppressedOrdinal) {
                next.suppressed = value.unsignedValue != 0;
            } else {
                next.firstAuthApplied = value.unsignedValue != 0;
            }
            relevant = true;
            break;
        default:
            break;
        }
    }
    if (!relevant) {
        return false;
    }
    if (next.revision != retained.revision || next.suppressed || !next.firstAuthApplied) {
        next.primary = -1.0F;
        next.secondary = -1.0F;
    }
    if (!next.suppressed && next.firstAuthApplied) {
        if (primary >= 0.0F) {
            next.primary = primary;
        }
        if (secondary >= 0.0F) {
            next.secondary = secondary;
        }
    }
    next.observed = true;
    const bool changed = next != retained;
    retained = next;
    return changed;
}

} // namespace sunrise::server::activity::mission
