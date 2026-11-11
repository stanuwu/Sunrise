#pragma once

#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/mission_effect_auth.h"
#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Last hop-on sensor level seen for one slot. */
struct EffectLevel final {
    std::int32_t revision{};
    bool attached{};
    /** A body reported either field, so the first report raises an event even at zero. */
    bool known{};
    bool operator==(const EffectLevel&) const = default;
};

/**
 * Merges one decoded type-26 body into the retained level.
 * @return True when the level changed.
 */
[[nodiscard]] inline bool update_effect_level(
    EffectLevel& level,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    namespace effect = middleware::bap::activity_message::mission_effect;
    EffectLevel next = level;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != root) {
            continue;
        }
        if (value.fieldOrdinal == effect::kSenseRevisionOrdinal) {
            next.revision = static_cast<std::int32_t>(value.signedValue);
            next.known = true;
        } else if (value.fieldOrdinal == effect::kSenseAttachedOrdinal) {
            next.attached = value.unsignedValue != 0;
            next.known = true;
        }
    }
    const bool changed = !(next == level);
    level = next;
    return changed;
}

} // namespace sunrise::server::activity::mission
