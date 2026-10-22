#include "../../middleware/gameplay/external/actor_entity_registry.h"
#include "../../state/activity_sdk/generated_world/runtime.h"
#include "activity_sdk_device_internal.h"
#include "activity_sdk_squad_runtime.h"

namespace sunrise::server::activity::activity_sdk_devices {
namespace {

/** The named actor's parent comes from its exact package source relation. */
[[nodiscard]] Status parent_definition(const state::activity_sdk::BoundView& view,
                                       std::uint32_t actorSlot,
                                       std::uint32_t& squadRow) noexcept {
    namespace sdk = state::activity_sdk;
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    if (view.catalog == nullptr || actorSlot >= view.catalog->slots().size()) {
        return Status::invalidSlot;
    }
    const auto& actor = view.catalog->slots()[actorSlot];
    if (actor.objectIndex >= view.catalog->objects().size()
        || actor.slotType != auth::kType2SlotType
        || actor.componentClass != auth::kType2ComponentClass
        || actor.authSchema != auth::kType2Schema || actor.senseSchema != auth::kType2SenseSchema) {
        return Status::invalidSlot;
    }
    sdk::generated_world::GeneratedWorldView world{};
    if (sdk::generated_world::resolve(view, world) != sdk::generated_world::BindStatus::ready
        || world.snapshot() == nullptr) {
        return Status::targetUnavailable;
    }
    const state::gameplay::entity_identity::ActorSourceReference reference{
        view.catalog->objects()[actor.objectIndex].objectKey,
        static_cast<std::uint16_t>(actor.slotIndex),
        static_cast<std::uint8_t>(actor.slotType),
        true,
        true};
    state::gameplay::entity_identity::ActorSourceReference source{};
    if (!middleware::gameplay::external::resolve_combatant_squad_source(
            *world.snapshot(), reference, source)) {
        return Status::targetUnavailable;
    }
    const auto* scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        return Status::invalidView;
    }
    squadRow = sdk::format::kAbsentIndex;
    const auto slots = view.catalog->slots();
    const auto objects = view.catalog->objects();
    const auto squads = view.catalog->squads();
    for (const auto& squad : sdk::scenario_squads(*view.catalog, *scenario)) {
        if (squad.slotIndex >= slots.size() || squad.objectIndex >= objects.size()) {
            return Status::invalidSlot;
        }
        const auto& slot = slots[squad.slotIndex];
        if (objects[squad.objectIndex].objectKey == source.key && slot.slotIndex == source.index
            && slot.slotType == source.type) {
            if (squadRow != sdk::format::kAbsentIndex) {
                return Status::ambiguousTarget;
            }
            squadRow = static_cast<std::uint32_t>(&squad - squads.data());
        }
    }
    return squadRow == sdk::format::kAbsentIndex ? Status::targetUnavailable : Status::ready;
}

/** Transport preparation uses the unique SDK parent selected by the actor descriptor. */
[[nodiscard]] Status parent(const state::activity_sdk::BoundView& view,
                            std::uint32_t actorSlot,
                            detail::PreparedDevice& output,
                            std::uint32_t& squadRow) noexcept {
    const auto resolved = parent_definition(view, actorSlot, squadRow);
    return resolved == Status::ready
               ? detail::prepare_slot(view, view.catalog->squads()[squadRow].slotIndex, output)
               : resolved;
}

/** Creation reserves authored candidates only when the parent has no usable reservation. */
[[nodiscard]] Status
ensure_source_reserved(const state::activity_sdk::BoundView& view,
                       const detail::PreparedDevice& source,
                       std::uint32_t squadRow,
                       const host::ScriptableOutputReservation& reservation) noexcept {
    namespace sdk = state::activity_sdk;
    namespace squad_auth = middleware::bap::activity_message::squad_auth;
    const auto sourceStatus = host::actor_program_source_status(view.binding, source.target);
    if (sourceStatus == host::ActorProgramSourceStatus::ready) {
        return Status::ready;
    }
    if (sourceStatus == host::ActorProgramSourceStatus::incompatible) {
        return Status::refused;
    }
    const auto members = sdk::squad_members(*view.catalog, view.catalog->squads()[squadRow]);
    std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength> counts{};
    if (members.empty() || members.size() > counts.size()) {
        return Status::invalidValue;
    }
    bool positive = false;
    for (std::size_t index = 0; index < members.size(); ++index) {
        counts[index] = members[index].defaultCount;
        if (counts[index] < 0) {
            return Status::invalidValue;
        }
        positive = positive || counts[index] > 0;
    }
    if (!positive) {
        return Status::invalidValue;
    }
    const auto status = activity_sdk_squads::place_reserved(view,
                                                            squadRow,
                                                            std::span(counts).first(members.size()),
                                                            squad_auth::Mode::reserve,
                                                            reservation);
    return status == activity_sdk_squads::Status::queued ? Status::queued : Status::refused;
}

} // namespace

/** Script decisions capture only the native parent row; its transport preparation stays private. */
Status actor_program_source_squad(const state::activity_sdk::BoundView& view,
                                  std::uint32_t actorSlot,
                                  std::uint32_t& squadRow) noexcept {
    squadRow = state::activity_sdk::format::kAbsentIndex;
    return parent_definition(view, actorSlot, squadRow);
}

/**
 * Population bookkeeping uses the exact SDK parent that the transported body names. The parent and
 * its wire ClientRef come from the package and the generated world alone, so the check holds on
 * every link, including after the carrier has left or moved.
 */
Status actor_program_parent_squad(const state::activity_sdk::BoundView& view,
                                  std::uint32_t actorSlot,
                                  const host::ScriptableTarget& expectedParent,
                                  std::uint32_t& squadRow) noexcept {
    squadRow = state::activity_sdk::format::kAbsentIndex;
    std::uint32_t row = state::activity_sdk::format::kAbsentIndex;
    const auto resolved = parent_definition(view, actorSlot, row);
    if (resolved != Status::ready) {
        return resolved;
    }
    const auto& squad = view.catalog->squads()[row];
    const auto& slot = view.catalog->slots()[squad.slotIndex];
    if (view.catalog->objects()[squad.objectIndex].objectKey != expectedParent.registryKey
        || slot.slotIndex != expectedParent.slotIndex || slot.slotType != expectedParent.slotType
        || slot.authSchema != expectedParent.authSchema) {
        return Status::targetUnavailable;
    }
    squadRow = row;
    return Status::ready;
}

/** Revalidates the pinned actor and its authored parent before reserving a native program. */
Status run_actor_program_reserved(const state::activity_sdk::BoundView& view,
                                  const state::activity::mission::TypedIntent& intent,
                                  const host::ScriptableOutputReservation& reservation) noexcept {
    const bool retire = intent.kind == state::activity::mission::IntentKind::retireActor;
    if ((!retire && intent.kind != state::activity::mission::IntentKind::runActorProgram)
        || (retire && intent.active) || intent.authByteCount != intent.authBody.size()) {
        return Status::invalidBody;
    }
    const auto validation = detail::validate_auth(view,
                                                  intent.firstRow,
                                                  intent.objectTag,
                                                  intent.registryKey,
                                                  intent.authSchema,
                                                  intent.slotIndex,
                                                  intent.slotType,
                                                  intent.authBody,
                                                  intent.authBitCount,
                                                  intent.sdkBuildSha256);
    if (validation != Status::ready) {
        return validation;
    }
    detail::PreparedDevice actor{}, source{};
    const auto prepared = detail::prepare_combatant(view, intent.firstRow, actor);
    if (prepared != Status::ready) {
        return prepared;
    }
    if (intent.active) {
        std::uint32_t squadRow = state::activity_sdk::format::kAbsentIndex;
        const auto resolved = parent(view, intent.firstRow, source, squadRow);
        if (resolved != Status::ready) {
            return resolved;
        }
        if (actor.activityClientGeneration != source.activityClientGeneration) {
            return Status::staleActivityClient;
        }
        if (squadRow != intent.secondRow) {
            return Status::invalidSlot;
        }
        const auto reserved = ensure_source_reserved(view, source, squadRow, reservation);
        if (reserved != Status::ready) {
            return reserved;
        }
    }
    return host::request_type2_program(view.binding,
                                       actor.target,
                                       source.target,
                                       actor.target.stateLocalRoster ? &actor.generatedRosterGroup
                                                                     : nullptr,
                                       intent.authBody,
                                       intent.authBitCount,
                                       intent.active,
                                       actor.activityClientGeneration,
                                       &reservation,
                                       retire)
               ? Status::queued
               : Status::refused;
}

} // namespace sunrise::server::activity::activity_sdk_devices
