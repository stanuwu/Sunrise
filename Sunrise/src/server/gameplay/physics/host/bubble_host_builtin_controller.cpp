#include <variant>

#include "command_effects.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                                     world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (world::equal_actor(candidate.key, actor)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] const BubbleHost::Storage::BodyBinding*
find_body(const BubbleHost::Storage::WorldSlot& slot, world::ActorKey actor) noexcept {
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            return &binding;
        }
    }
    return nullptr;
}

/** @return Exact registered controller profile, or null. */
[[nodiscard]] const controller::ControllerProfile*
find_profile(const BubbleHost::Storage::WorldSlot& slot, world::BlueprintRef profile) noexcept {
    for (const BubbleHost::Storage::ControllerProfileSlot& candidate : slot.controllerProfiles) {
        if (candidate.occupied && candidate.profile.controller.id == profile.id
            && candidate.profile.controller.version == profile.version) {
            return &candidate.profile;
        }
    }
    return nullptr;
}

} // namespace

/** Configures one built-in controller after all host-side preflight checks. */
bool execute_builtin_controller_command(BubbleHost::Storage::WorldSlot& slot,
                                        const world::HostExecutionContext& context,
                                        const world::HostCommand& command,
                                        std::span<const world::ActorSnapshot> actors,
                                        EventWriter& writer) noexcept {
    const auto& configure = std::get<world::ConfigureControllerCommand>(command.payload);
    const world::ActorSnapshot* actor = find_actor(actors, configure.actor);
    const BubbleHost::Storage::BodyBinding* bodyBinding = find_body(slot, configure.actor);
    const controller::ControllerProfile* profile = find_profile(slot, configure.controller);
    backend::BodyState body{};
    if (actor == nullptr || bodyBinding == nullptr || profile == nullptr
        || !slot.physics.read_body(bodyBinding->body, body)) {
        return false;
    }
    controller::ControllerBinding binding{};
    binding.actor = configure.actor;
    binding.body = bodyBinding->body;
    binding.navigationScene = slot.navigationScene;
    binding.navigationAgent.stableOwnerId = configure.actor.id;
    binding.navigationAgent.shapeId = body.spec.shapeId;
    binding.navigationAgent.gate = body.spec.gate;
    binding.navigationAgent.shape = body.spec.shape;
    binding.navigationAgent.ignoreOwnerId = configure.actor.id;
    binding.bubble = actor->bubble;
    const controller::ControllerOperationResult configured =
        slot.builtInController.configure_controller(configure, binding, *profile);
    if (!configured.succeeded()) {
        return false;
    }
    world::CommittedEvent event{};
    event.actor = actor->key;
    event.transform = actor->transform;
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.actorRevision = actor->revision;
    event.authorityGeneration = actor->authority.generation;
    event.value = (static_cast<std::uint64_t>(configured.controller.generation) << 16U)
                  | configured.controller.slot;
    event.kind = world::CommittedEventKind::controllerConfigured;
    return writer.append(event);
}

} // namespace sunrise::server::gameplay::physics::host::detail
