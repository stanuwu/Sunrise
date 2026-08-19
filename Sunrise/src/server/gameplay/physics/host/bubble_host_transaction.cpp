#include "internal.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

/** Rebinds navigation after restoring its owning physics backend. */
[[nodiscard]] bool rebind_navigation(BubbleHost::Storage::WorldSlot& slot) noexcept {
    backend::SceneState scene{};
    if (!slot.physics.read_scene(slot.physicsScene, scene)) {
        return false;
    }
    backend::NavigationBackend navigation{};
    navigation.initialize(slot.physics);
    backend::NavigationSceneSpec spec{};
    spec.stableSceneId = scene.spec.stableSceneId;
    spec.contentBuild = scene.spec.contentBuild;
    spec.physicsScene = slot.physicsScene;
    spec.bubble = scene.spec.bubble;
    backend::NavigationSceneHandle handle{};
    if (!navigation.load_scene(spec, handle) || handle != slot.navigationScene) {
        return false;
    }
    slot.navigation = navigation;
    return true;
}

} // namespace

/** Captures every generic service mutation boundary before a fixed tick. */
void capture_tick_backup(BubbleHost::Storage::WorldSlot& slot) noexcept {
    BubbleHost::Storage::TickBackup& backup = slot.tickBackup;
    backup.physics = slot.physics;
    backup.timers = slot.timers;
    backup.triggers = slot.triggers;
    backup.objectives = slot.objectives;
    backup.credits = slot.credits;
    backup.combat = slot.combat;
    backup.builtInController = slot.builtInController;
    backup.motion = slot.motion;
    backup.bodies = slot.bodies;
    backup.triggerBindings = slot.triggerBindings;
    backup.combatants = slot.combatants;
    backup.paths = slot.paths;
    backup.triggerEvents = slot.triggerEvents;
    backup.timerEvents = slot.timerEvents;
    backup.poseCommits = slot.poseCommits;
    backup.contacts = slot.contacts;
    backup.serviceCommandTick = slot.serviceCommandTick;
    backup.queryCommandCount = slot.queryCommandCount;
    backup.timerCommandCount = slot.timerCommandCount;
    backup.triggerCommandCount = slot.triggerCommandCount;
    backup.objectiveCommandCount = slot.objectiveCommandCount;
    backup.creditCommandCount = slot.creditCommandCount;
    backup.combatCommandCount = slot.combatCommandCount;
    backup.triggerEventCount = slot.triggerEventCount;
    backup.timerEventCount = slot.timerEventCount;
    backup.poseCommitCount = slot.poseCommitCount;
    backup.healthy = slot.healthy;
    backup.valid = true;
}

/** Restores the exact pre-tick generic service and backend boundary. */
bool restore_tick_backup(BubbleHost::Storage::WorldSlot& slot) noexcept {
    BubbleHost::Storage::TickBackup& backup = slot.tickBackup;
    if (!backup.valid) {
        return false;
    }
    slot.physics = backup.physics;
    slot.timers = backup.timers;
    slot.triggers = backup.triggers;
    slot.objectives = backup.objectives;
    slot.credits = backup.credits;
    slot.combat = backup.combat;
    slot.motion = backup.motion;
    slot.bodies = backup.bodies;
    slot.triggerBindings = backup.triggerBindings;
    slot.combatants = backup.combatants;
    slot.paths = backup.paths;
    slot.triggerEvents = backup.triggerEvents;
    slot.timerEvents = backup.timerEvents;
    slot.poseCommits = backup.poseCommits;
    slot.contacts = backup.contacts;
    slot.serviceCommandTick = backup.serviceCommandTick;
    slot.queryCommandCount = backup.queryCommandCount;
    slot.timerCommandCount = backup.timerCommandCount;
    slot.triggerCommandCount = backup.triggerCommandCount;
    slot.objectiveCommandCount = backup.objectiveCommandCount;
    slot.creditCommandCount = backup.creditCommandCount;
    slot.combatCommandCount = backup.combatCommandCount;
    slot.triggerEventCount = backup.triggerEventCount;
    slot.timerEventCount = backup.timerEventCount;
    slot.poseCommitCount = backup.poseCommitCount;
    slot.healthy = backup.healthy;
    backup.valid = false;
    if (!rebind_navigation(slot)) {
        return false;
    }
    slot.builtInController = backup.builtInController;
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
