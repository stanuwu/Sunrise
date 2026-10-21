#pragma once

#include <cstdint>

#include "../host_runtime.h"

namespace sunrise::server::activity::mission {

/** Entity pools are polled ten times a second: above any authored gate, below the packet rate. */
inline constexpr std::uint64_t kEntityDamagePollIntervalMs = 100;
/** Normalised level a script reads for a pool the client has not reported. */
inline constexpr float kUnknownDamageLevel = -1.0F;

/** A queued observation may outlive its peer view, allocation or authored source. */
[[nodiscard]] inline bool
matches_entity_damage(const host::Event& event,
                      const state::gameplay::entity_identity::Identity& current) noexcept {
    return event.damageFromEntity && current.known && current.present && !current.conflicted
           && current.type == 0 && current.token == event.damageEntityLifetime.token
           && current.allocationSequence == event.damageEntityLifetime.allocationSequence
           && current.allocationEpoch == event.damageEntityLifetime.allocationEpoch
           && current.allocationDomain == event.damageEntityLifetime.allocationDomain
           && current.actorSource.known && current.actorSource.present
           && current.actorSource.key == event.firstRegistryKey
           && current.actorSource.index == event.firstSlotIndex
           && current.actorSource.type == event.firstSlotType;
}

} // namespace sunrise::server::activity::mission
