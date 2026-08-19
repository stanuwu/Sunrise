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

/** @return Canonical command-caused navigation event. */
[[nodiscard]] world::CommittedEvent command_event(const world::HostExecutionContext& context,
                                                  const world::HostCommand& command,
                                                  const world::ActorSnapshot* actor,
                                                  world::CommittedEventKind kind,
                                                  std::uint64_t value) noexcept {
    world::CommittedEvent event{};
    if (actor != nullptr) {
        event.actor = actor->key;
        event.transform = actor->transform;
        event.actorRevision = actor->revision;
        event.authorityGeneration = actor->authority.generation;
    }
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.value = value;
    event.kind = kind;
    return event;
}

/** Replaces or clears one actor's latest bounded navigation path. */
[[nodiscard]] bool retain_path(BubbleHost::Storage::WorldSlot& slot,
                               world::ActorKey actor,
                               backend::NavigationStatus status,
                               const backend::NavigationPath& path) noexcept {
    BubbleHost::Storage::PathBinding* freeSlot = nullptr;
    for (BubbleHost::Storage::PathBinding& binding : slot.paths) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            if (status == backend::NavigationStatus::success) {
                binding.path = path;
            } else {
                binding = {};
            }
            return true;
        }
        if (!binding.occupied && freeSlot == nullptr) {
            freeSlot = &binding;
        }
    }
    if (status != backend::NavigationStatus::success) {
        return true;
    }
    if (freeSlot == nullptr) {
        return false;
    }
    freeSlot->actor = actor;
    freeSlot->path = path;
    freeSlot->occupied = true;
    return true;
}

/** Resolves one path synchronously before runner command retention. */
[[nodiscard]] bool apply_path_request(BubbleHost::Storage::WorldSlot& slot,
                                      const world::HostExecutionContext& context,
                                      const world::HostCommand& command,
                                      std::span<const world::ActorSnapshot> actors,
                                      EventWriter& writer) noexcept {
    const auto& request = std::get<world::RequestPathCommand>(command.payload);
    const world::ActorSnapshot* actor = find_actor(actors, request.actor);
    const BubbleHost::Storage::BodyBinding* binding = find_body(slot, request.actor);
    backend::NavigationPath path{};
    backend::NavigationStatus status = backend::NavigationStatus::invalidQuery;
    if (actor != nullptr && binding != nullptr) {
        backend::BodyState body{};
        if (!slot.physics.read_body(binding->body, body)) {
            return false;
        }
        backend::ReachabilityQuery query{};
        query.start.scene = slot.navigationScene;
        query.start.bubble = actor->bubble;
        query.start.position = body.spec.transform.position;
        query.goal.scene = slot.navigationScene;
        query.goal.bubble = actor->bubble;
        query.goal.position = {request.destination.x, request.destination.y, request.destination.z};
        query.agent.stableOwnerId = request.actor.id;
        query.agent.shapeId = body.spec.shapeId;
        query.agent.gate = body.spec.gate;
        query.agent.shape = body.spec.shape;
        query.agent.ignoreOwnerId = request.actor.id;
        status = slot.navigation.find_path(query, path);
    }
    if (!retain_path(slot, request.actor, status, path)) {
        return false;
    }
    controller::ControllerHandle controllerHandle{};
    if (slot.builtInController.find_controller(request.actor, controllerHandle)) {
        const controller::ControllerOperationResult controllerResult =
            slot.builtInController.request_path(request, context.tick);
        if (controllerResult.status == controller::ControllerStatus::unhealthy
            || controllerResult.status == controller::ControllerStatus::notInitialized) {
            return false;
        }
    }
    return writer.append(command_event(context,
                                       command,
                                       actor,
                                       world::CommittedEventKind::pathRequested,
                                       static_cast<std::uint64_t>(status)));
}

} // namespace

/** Applies one path or controller command on the command-commit boundary. */
bool execute_navigation_command(BubbleHost::Storage::WorldSlot& slot,
                                const world::HostExecutionContext& context,
                                const world::HostCommand& command,
                                std::span<const world::ActorSnapshot> actors,
                                EventWriter& writer) noexcept {
    if (world::command_kind(command) == world::HostCommandKind::requestPath) {
        return apply_path_request(slot, context, command, actors, writer);
    }
    if (world::command_kind(command) != world::HostCommandKind::configureController) {
        return false;
    }
    if (slot.controller == nullptr) {
        return execute_builtin_controller_command(slot, context, command, actors, writer);
    }
    const std::span<world::CommittedEvent> output = writer.remaining();
    const world::HostCommandExecutionResult result =
        slot.controller->configure(context, command, actors, slot.physics, slot.navigation, output);
    if (result.status != world::HostCommandExecutionStatus::applied
        || result.eventCount > output.size()) {
        return false;
    }
    writer.count += result.eventCount;
    return true;
}

/** Runs periodic controller work without replaying command mutations. */
bool run_controller_navigation(BubbleHost::Storage::WorldSlot& slot,
                               const world::HostExecutionContext& context,
                               std::span<const world::ActorSnapshot> actors,
                               std::span<const world::CommittedEvent> committedEvents,
                               EventWriter& writer) noexcept {
    const controller::ControllerStepResult builtIn =
        slot.builtInController.step(context.tick, actors);
    if (!builtIn.healthy) {
        return false;
    }
    for (const controller::ControllerEvent& source : slot.builtInController.events()) {
        world::CommittedEvent event{};
        event.actor = source.actor;
        event.commandId = 0;
        event.tick = context.tick;
        event.actorRevision = source.sequence;
        event.value = static_cast<std::uint64_t>(source.kind);
        event.kind = world::CommittedEventKind::serviceTickCommitted;
        if (!writer.append(event)) {
            return false;
        }
    }
    slot.builtInController.clear_events();
    if (slot.controller == nullptr) {
        return true;
    }
    const std::span<world::CommittedEvent> output = writer.remaining();
    const world::HostTickExecutionResult result = slot.controller->step(
        context, actors, committedEvents, slot.physics, slot.navigation, output);
    if (!result.healthy || result.eventCount > output.size()) {
        return false;
    }
    writer.count += result.eventCount;
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
