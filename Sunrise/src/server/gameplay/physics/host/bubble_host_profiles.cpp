#include <cmath>

#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {
namespace {

[[nodiscard]] bool same_blueprint(world::BlueprintRef left, world::BlueprintRef right) noexcept {
    return left.id == right.id && left.version == right.version;
}

/** @return True for finite, bounded steering build data. */
[[nodiscard]] bool valid_controller_profile(const controller::ControllerProfile& profile) noexcept {
    return world::valid_blueprint(profile.controller)
           && world::valid_blueprint(profile.navigationProfile) && std::isfinite(profile.maxSpeed)
           && profile.maxSpeed > 0.0F && std::isfinite(profile.maxAcceleration)
           && profile.maxAcceleration > 0.0F && std::isfinite(profile.maxTurnRadiansPerSecond)
           && profile.maxTurnRadiansPerSecond >= 0.0F && std::isfinite(profile.arrivalRadius)
           && profile.arrivalRadius > 0.0F && std::isfinite(profile.waypointRadius)
           && profile.waypointRadius >= profile.arrivalRadius
           && std::isfinite(profile.progressEpsilon) && profile.progressEpsilon > 0.0F
           && profile.stuckTickLimit != 0
           && (profile.facing == controller::FacingMode::none
               || profile.maxTurnRadiansPerSecond > 0.0F);
}

} // namespace

/** Registers one immutable combat profile in fixed world storage. */
HostStatus BubbleHost::register_combat_profile(WorldHandle handle,
                                               const CombatProfile& profile) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    if (!world::valid_blueprint(profile.profile) || profile.policyOwnerId == 0
        || profile.maximumHealth == 0 || profile.faction >= 64) {
        return HostStatus::invalidArgument;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    Storage::CombatProfileSlot* freeSlot = nullptr;
    for (Storage::CombatProfileSlot& candidate : slot.combatProfiles) {
        if (candidate.occupied && same_blueprint(candidate.profile.profile, profile.profile)) {
            return HostStatus::alreadyOpen;
        }
        if (!candidate.occupied && freeSlot == nullptr) {
            freeSlot = &candidate;
        }
    }
    if (freeSlot == nullptr) {
        return HostStatus::capacity;
    }
    freeSlot->profile = profile;
    freeSlot->occupied = true;
    return HostStatus::success;
}

/** Registers one immutable attack profile and combat definition. */
HostStatus BubbleHost::register_attack_profile(WorldHandle handle,
                                               const AttackProfile& profile) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    if (!world::valid_blueprint(profile.attack) || profile.policyOwnerId == 0 || profile.damage == 0
        || profile.maximumRangeMillimeters > mechanics::kCombatRangeLimitMillimeters) {
        return HostStatus::invalidArgument;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    Storage::AttackProfileSlot* freeSlot = nullptr;
    for (Storage::AttackProfileSlot& candidate : slot.attackProfiles) {
        if (candidate.occupied && same_blueprint(candidate.profile.attack, profile.attack)) {
            return HostStatus::alreadyOpen;
        }
        if (!candidate.occupied && freeSlot == nullptr) {
            freeSlot = &candidate;
        }
    }
    if (freeSlot == nullptr) {
        return HostStatus::capacity;
    }

    mechanics::AttackBlueprintDefinition definition{};
    definition.attackBlueprintId = profile.attack.id;
    definition.policyOwnerId = profile.policyOwnerId;
    definition.blueprintVersion = profile.attack.version;
    definition.damage = profile.damage;
    definition.maximumRangeMillimeters = profile.maximumRangeMillimeters;
    definition.cooldownTicks = profile.cooldownTicks;
    mechanics::AttackBlueprintHandle attack{};
    if (slot.combat.create_attack_blueprint(definition, attack)
        != mechanics::CombatResult::applied) {
        return HostStatus::serviceRejected;
    }
    freeSlot->profile = profile;
    freeSlot->attack = attack;
    freeSlot->occupied = true;
    return HostStatus::success;
}

/** Registers one immutable built-in steering profile. */
HostStatus
BubbleHost::register_controller_profile(WorldHandle handle,
                                        const controller::ControllerProfile& profile) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    if (!valid_controller_profile(profile)) {
        return HostStatus::invalidArgument;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    Storage::ControllerProfileSlot* freeSlot = nullptr;
    for (Storage::ControllerProfileSlot& candidate : slot.controllerProfiles) {
        if (candidate.occupied
            && same_blueprint(candidate.profile.controller, profile.controller)) {
            return HostStatus::alreadyOpen;
        }
        if (!candidate.occupied && freeSlot == nullptr) {
            freeSlot = &candidate;
        }
    }
    if (freeSlot == nullptr) {
        return HostStatus::capacity;
    }
    freeSlot->profile = profile;
    freeSlot->occupied = true;
    return HostStatus::success;
}

/** Creates one objective within the selected policy's fixed budget. */
HostStatus BubbleHost::create_objective(WorldHandle handle,
                                        const mechanics::ObjectiveCounterDefinition& definition,
                                        mechanics::ObjectiveCounterHandle& counter) noexcept {
    counter = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    const mechanics::ObjectiveLedgerSnapshot snapshot = slot.objectives.snapshot();
    std::size_t count = 0;
    for (const mechanics::ObjectiveCounterSnapshot& item : snapshot.counters) {
        count += item.active ? 1U : 0U;
    }
    if (count >= slot.counterBudget) {
        return HostStatus::capacity;
    }
    const mechanics::ObjectiveResult result = slot.objectives.create_counter(definition, counter);
    return result == mechanics::ObjectiveResult::applied ? HostStatus::success
                                                         : HostStatus::serviceRejected;
}

/** Creates one participant within the selected policy's fixed budget. */
HostStatus
BubbleHost::create_participant(WorldHandle handle,
                               const mechanics::ParticipantCreditDefinition& definition,
                               mechanics::ParticipantCreditHandle& participant) noexcept {
    participant = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    const mechanics::ParticipantCreditLedgerSnapshot snapshot = slot.credits.snapshot();
    std::size_t count = 0;
    for (const mechanics::ParticipantCreditSnapshot& item : snapshot.participants) {
        count += item.active ? 1U : 0U;
    }
    if (count >= slot.creditBudget) {
        return HostStatus::capacity;
    }
    const mechanics::ParticipantCreditResult result =
        slot.credits.create_participant(definition, participant);
    return result == mechanics::ParticipantCreditResult::applied ? HostStatus::success
                                                                 : HostStatus::serviceRejected;
}

/** Adds one incident to the fixed fail-closed presentation allowlist. */
HostStatus BubbleHost::allow_incident(WorldHandle handle, std::uint64_t incidentId) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    if (incidentId == 0) {
        return HostStatus::invalidArgument;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    for (std::size_t index = 0; index < slot.incidentCount; ++index) {
        if (slot.incidentAllowlist[index] == incidentId) {
            return HostStatus::success;
        }
    }
    if (slot.incidentCount == slot.incidentAllowlist.size()) {
        return HostStatus::capacity;
    }
    slot.incidentAllowlist[slot.incidentCount++] = incidentId;
    return HostStatus::success;
}

} // namespace sunrise::server::gameplay::physics::host
