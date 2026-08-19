#include <cmath>

#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {
namespace {

/** @return Inert manifest with bounded direct-host generic services. */
[[nodiscard]] world::ActivityPolicyManifest
scriptless_manifest(const WorldOpenRequest& request) noexcept {
    world::ActivityPolicyManifest manifest{};
    manifest.policy = {0x53524350544C4553ULL, 1};
    manifest.contentBuildId = request.scene.contentBuild;
    manifest.fixedRateHz = world::kDefaultFixedRateHz;
    manifest.triggerBudget = request.scriptlessTriggerBudget;
    manifest.queryBudget = request.scriptlessQueryBudget;
    manifest.counterBudget = request.scriptlessCounterBudget;
    manifest.creditBudget = request.scriptlessCreditBudget;
    manifest.persistenceSchemaVersion = 1;
    return manifest;
}

} // namespace

/** Opens all logical and backend services as one rollback-safe transaction. */
HostStatus BubbleHost::open_world(const WorldOpenRequest& request,
                                  world::IActivityPolicy* policy,
                                  WorldHandle& handle) noexcept {
    handle = {};
    if (!ready()) {
        return HostStatus::unavailable;
    }
    if (request.activitySessionId == 0 || request.ownerEpoch == 0
        || !std::isfinite(request.millimetersPerUnit) || request.millimetersPerUnit < 0.0F) {
        return HostStatus::invalidArgument;
    }
    const std::uint64_t logicalWorldId =
        request.logicalWorldId == 0 ? request.activitySessionId : request.logicalWorldId;
    const world::ActivityPolicyManifest selectedManifest =
        policy == nullptr ? scriptless_manifest(request) : policy->manifest();
    if (selectedManifest.contentBuildId != request.scene.contentBuild
        || selectedManifest.triggerBudget > mechanics::kTriggerCapacity
        || selectedManifest.queryBudget > backend::kQueryHitCapacity
        || selectedManifest.counterBudget > mechanics::kObjectiveCounterCapacity
        || selectedManifest.creditBudget > mechanics::kParticipantCreditCapacity) {
        return HostStatus::policyRejected;
    }
    for (std::size_t index = 0; index < storage_->worlds.size(); ++index) {
        const Storage::WorldSlot& slot = storage_->worlds[index];
        if (slot.active && slot.runner.identity().activitySessionId == request.activitySessionId) {
            return HostStatus::alreadyOpen;
        }
    }

    std::size_t slotIndex = storage_->worlds.size();
    for (std::size_t index = 0; index < storage_->worlds.size(); ++index) {
        const Storage::WorldSlot& slot = storage_->worlds[index];
        if (!slot.active && !slot.retired) {
            slotIndex = index;
            break;
        }
    }
    if (slotIndex == storage_->worlds.size()) {
        return HostStatus::capacity;
    }

    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    slot.physics.initialize();
    if (!slot.physics.load_scene(request.scene, slot.physicsScene)) {
        slot.physics.shutdown();
        return HostStatus::backendRejected;
    }
    slot.navigation.initialize(slot.physics);
    backend::NavigationSceneSpec navigationSpec{};
    navigationSpec.stableSceneId = request.scene.stableSceneId;
    navigationSpec.contentBuild = request.scene.contentBuild;
    navigationSpec.physicsScene = slot.physicsScene;
    navigationSpec.bubble = request.scene.bubble;
    if (!slot.navigation.load_scene(navigationSpec, slot.navigationScene)) {
        slot.navigation.shutdown();
        slot.physics.shutdown();
        return HostStatus::backendRejected;
    }

    world::WorldOpenConfig config{};
    config.executor = this;
    config.activitySessionId = request.activitySessionId;
    config.ownerGeneration = request.ownerEpoch;
    config.deterministicSeed = request.deterministicSeed;
    config.allowHostActorCommands = request.allowHostActorCommands;
    world::IActivityPolicy* selectedPolicy = policy;
    if (selectedPolicy == nullptr) {
        slot.fallbackPolicy.configure(selectedManifest);
        selectedPolicy = &slot.fallbackPolicy;
    }
    if (!slot.runner.open(config, selectedPolicy)) {
        slot.navigation.shutdown();
        slot.physics.shutdown();
        return HostStatus::policyRejected;
    }
    const float fixedDt = 1.0F / static_cast<float>(selectedManifest.fixedRateHz);
    if (!slot.builtInController.initialize(slot.physics, slot.navigation, fixedDt)) {
        slot.runner.close();
        slot.navigation.shutdown();
        slot.physics.shutdown();
        return HostStatus::backendRejected;
    }

    const mechanics::WorldEpoch epoch = slot.runner.identity().worldGeneration;
    slot.timers.reset(epoch);
    slot.triggers.reset(epoch);
    slot.objectives.reset(epoch);
    slot.credits.reset(epoch);
    slot.combat.reset(epoch);
    slot.interest.reset();
    slot.motion.reset();
    slot.bodies = {};
    slot.triggerBindings = {};
    slot.combatProfiles = {};
    slot.controllerProfiles = {};
    slot.attackProfiles = {};
    slot.combatants = {};
    slot.paths = {};
    slot.poseCommits = {};
    slot.tickBackup.valid = false;
    slot.triggerEvents = {};
    slot.timerEvents = {};
    slot.checkpointRequests = {};
    slot.incidentAllowlist = {};
    slot.tickStages = {};
    slot.contacts = {};
    slot.checkpointSink = request.checkpointSink;
    slot.controller = request.controller;
    slot.logicalWorldId = logicalWorldId;
    std::uint64_t retainedFloor = 0;
    slot.journalCursor = 0;
    static_cast<void>(
        storage_->journal->cursors(logicalWorldId, retainedFloor, slot.journalCursor));
    slot.lastOwnerEpoch = request.ownerEpoch;
    slot.serviceCommandTick = 0;
    slot.queryCommandCount = 0;
    slot.timerCommandCount = 0;
    slot.triggerCommandCount = 0;
    slot.objectiveCommandCount = 0;
    slot.creditCommandCount = 0;
    slot.combatCommandCount = 0;
    slot.poseCommitCount = 0;
    slot.triggerEventCount = 0;
    slot.timerEventCount = 0;
    slot.checkpointRequestCount = 0;
    slot.incidentCount = 0;
    slot.tickStageCount = 0;
    slot.triggerBudget = selectedManifest.triggerBudget;
    slot.queryBudget = selectedManifest.queryBudget;
    slot.counterBudget = selectedManifest.counterBudget;
    slot.creditBudget = selectedManifest.creditBudget;
    slot.millimetersPerUnit = request.millimetersPerUnit;
    slot.active = true;
    slot.healthy = true;
    slot.inTick = false;
    ++storage_->worldCount;
    handle = detail::make_handle(slotIndex, slot.handleGeneration);
    return HostStatus::success;
}

/** Closes an exact world and invalidates every subordinate lifetime. */
HostStatus BubbleHost::close_world(WorldHandle handle) noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (slot.inTick) {
        return HostStatus::unhealthy;
    }
    const bool peersClean = detail::reset_peer_services(slot);
    slot.active = false;
    slot.healthy = false;
    slot.runner.close();
    slot.builtInController.shutdown();
    slot.navigation.shutdown();
    slot.physics.shutdown();
    slot.timers.reset(mechanics::kAbsentWorldEpoch);
    slot.triggers.reset(mechanics::kAbsentWorldEpoch);
    slot.objectives.reset(mechanics::kAbsentWorldEpoch);
    slot.credits.reset(mechanics::kAbsentWorldEpoch);
    slot.combat.reset(mechanics::kAbsentWorldEpoch);
    slot.bodies = {};
    slot.triggerBindings = {};
    slot.combatProfiles = {};
    slot.controllerProfiles = {};
    slot.attackProfiles = {};
    slot.combatants = {};
    slot.paths = {};
    slot.poseCommits = {};
    slot.tickBackup.valid = false;
    slot.checkpointRequests = {};
    slot.checkpointSink = nullptr;
    slot.controller = nullptr;
    slot.logicalWorldId = 0;
    slot.journalCursor = 0;
    slot.serviceCommandTick = 0;
    slot.queryCommandCount = 0;
    slot.timerCommandCount = 0;
    slot.triggerCommandCount = 0;
    slot.objectiveCommandCount = 0;
    slot.creditCommandCount = 0;
    slot.combatCommandCount = 0;
    slot.poseCommitCount = 0;
    slot.checkpointRequestCount = 0;
    slot.triggerBudget = 0;
    slot.queryBudget = 0;
    slot.counterBudget = 0;
    slot.creditBudget = 0;
    slot.inTick = false;
    detail::retire_handle_generation(slot.handleGeneration, slot.retired);
    --storage_->worldCount;
    return peersClean ? HostStatus::success : HostStatus::unhealthy;
}

/** Replaces one exact failed lifetime with the scriptless null-policy fallback. */
HostStatus BubbleHost::restart_empty(WorldHandle handle,
                                     const WorldOpenRequest& request,
                                     WorldHandle& replacement) noexcept {
    replacement = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    const world::WorldIdentity identity = slot.runner.identity();
    if (request.activitySessionId != identity.activitySessionId) {
        return HostStatus::invalidArgument;
    }
    const std::uint64_t logicalWorldId =
        request.logicalWorldId == 0 ? request.activitySessionId : request.logicalWorldId;
    if (logicalWorldId != slot.logicalWorldId) {
        return HostStatus::invalidArgument;
    }
    if (request.ownerEpoch <= identity.ownerGeneration) {
        return HostStatus::staleOwner;
    }
    const HostStatus closeStatus = close_world(handle);
    if (closeStatus != HostStatus::success) {
        return closeStatus;
    }
    return open_world(request, nullptr, replacement);
}

} // namespace sunrise::server::gameplay::physics::host
