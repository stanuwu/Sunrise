#include <cstdint>
#include <variant>

#include "command_effects.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] mechanics::CommandHeader
mechanics_header(const world::HostExecutionContext& context,
                 const world::HostCommand& command) noexcept {
    return {context.world.worldGeneration,
            command.header.commandId,
            context.tick,
            command.header.policyOwnerId,
            command.header.definition.version};
}

[[nodiscard]] backend::Transform to_backend(const world::Transform& transform) noexcept {
    return {
        {transform.position.x, transform.position.y, transform.position.z},
        {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w}};
}

[[nodiscard]] std::uint32_t shape_id(world::BlueprintRef blueprint) noexcept {
    const std::uint64_t folded = blueprint.id ^ (blueprint.id >> 32U) ^ blueprint.version;
    const std::uint32_t value = static_cast<std::uint32_t>(folded);
    return value == 0 ? 1U : value;
}

/** @return Canonical command-caused trigger event. */
[[nodiscard]] world::CommittedEvent command_event(const world::HostExecutionContext& context,
                                                  const world::HostCommand& command,
                                                  world::CommittedEventKind kind,
                                                  std::uint64_t value) noexcept {
    world::CommittedEvent event{};
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.value = value;
    event.kind = kind;
    return event;
}

/** Creates matching logical and backend trigger state. */
[[nodiscard]] bool create_trigger(BubbleHost::Storage::WorldSlot& slot,
                                  const world::HostExecutionContext& context,
                                  const world::HostCommand& command,
                                  EventWriter& writer) noexcept {
    const auto& create = std::get<world::CreateTriggerCommand>(command.payload);
    BubbleHost::Storage::TriggerBinding* freeSlot = nullptr;
    for (BubbleHost::Storage::TriggerBinding& binding : slot.triggerBindings) {
        if (binding.occupied && binding.triggerId == create.triggerId) {
            return false;
        }
        if (!binding.occupied && freeSlot == nullptr) {
            freeSlot = &binding;
        }
    }
    if (freeSlot == nullptr) {
        return false;
    }
    backend::SceneState scene{};
    if (!slot.physics.read_scene(slot.physicsScene, scene)) {
        return false;
    }
    backend::BodySpec bodySpec{};
    bodySpec.scene = slot.physicsScene;
    bodySpec.stableOwnerId = create.triggerId;
    bodySpec.shapeId = shape_id(create.triggerProfile);
    bodySpec.gate.bubble = scene.spec.bubble;
    bodySpec.gate.filterMask = backend::kAllCollisionLayers;
    bodySpec.shape.kind = backend::ShapeKind::box;
    bodySpec.shape.halfExtents = {create.halfExtents.x, create.halfExtents.y, create.halfExtents.z};
    bodySpec.transform = to_backend(create.transform);
    bodySpec.motion = backend::MotionKind::staticBody;
    bodySpec.isTrigger = true;
    backend::BodyHandle body{};
    if (!slot.physics.create_body(bodySpec, body)) {
        return false;
    }
    mechanics::TriggerHandle trigger{};
    const mechanics::TriggerResult result = slot.triggers.create(
        mechanics_header(context, command), create.triggerId, scene.spec.bubble, trigger);
    if (result != mechanics::TriggerResult::applied) {
        static_cast<void>(slot.physics.destroy_body(body));
        return false;
    }
    freeSlot->trigger = trigger;
    freeSlot->body = body;
    freeSlot->triggerId = create.triggerId;
    freeSlot->occupied = true;
    world::CommittedEvent event = command_event(
        context, command, world::CommittedEventKind::triggerCreated, create.triggerId);
    event.actorRevision = trigger.generation;
    return writer.append(event);
}

/** Removes matching logical and backend trigger state. */
[[nodiscard]] bool remove_trigger(BubbleHost::Storage::WorldSlot& slot,
                                  const world::HostExecutionContext& context,
                                  const world::HostCommand& command,
                                  EventWriter& writer) noexcept {
    const auto& remove = std::get<world::RemoveTriggerCommand>(command.payload);
    const mechanics::TriggerSystemSnapshot snapshot = slot.triggers.snapshot();
    mechanics::TriggerHandle handle{};
    bool found = false;
    for (std::size_t index = 0; index < snapshot.slots.size(); ++index) {
        const mechanics::TriggerSlotSnapshot& trigger = snapshot.slots[index];
        if (trigger.active && trigger.triggerId == remove.triggerId
            && trigger.generation == remove.triggerGeneration) {
            handle = {trigger.generation, static_cast<std::uint16_t>(index)};
            found = true;
            break;
        }
    }
    if (!found
        || slot.triggers.remove(mechanics_header(context, command), handle)
               != mechanics::TriggerResult::applied) {
        return false;
    }
    for (BubbleHost::Storage::TriggerBinding& binding : slot.triggerBindings) {
        if (!binding.occupied || binding.triggerId != remove.triggerId
            || binding.trigger.generation != remove.triggerGeneration) {
            continue;
        }
        if (!slot.physics.destroy_body(binding.body)) {
            return false;
        }
        binding = {};
        break;
    }
    world::CommittedEvent event = command_event(
        context, command, world::CommittedEventKind::triggerRemoved, remove.triggerId);
    event.actorRevision = remove.triggerGeneration;
    return writer.append(event);
}

} // namespace

/** Applies one trigger lifecycle command before periodic physics work. */
bool execute_trigger_command(BubbleHost::Storage::WorldSlot& slot,
                             const world::HostExecutionContext& context,
                             const world::HostCommand& command,
                             EventWriter& writer) noexcept {
    if (world::command_kind(command) == world::HostCommandKind::createTrigger) {
        return create_trigger(slot, context, command, writer);
    }
    return world::command_kind(command) == world::HostCommandKind::removeTrigger
           && remove_trigger(slot, context, command, writer);
}

} // namespace sunrise::server::gameplay::physics::host::detail
