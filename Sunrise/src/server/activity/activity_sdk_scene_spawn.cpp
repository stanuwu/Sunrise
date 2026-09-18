#include "activity_sdk_scene_spawn.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../state/activity_sdk/generated_world/runtime.h"
#include "activity_sdk_device_internal.h"
#include "activity_sdk_mission_internal.h"
#include "activity_sdk_squad_runtime.h"

namespace sunrise::server::activity::activity_sdk_mission {
namespace {

namespace sdk = state::activity_sdk;
namespace devices = activity_sdk_devices;
namespace squad_auth = middleware::bap::activity_message::squad_auth;
using Counts = std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength>;

/** Package defaults must supply a positive bounded reservation for a missing source. */
[[nodiscard]] bool default_counts(const sdk::Catalog& catalog,
                                  std::uint32_t squadRow,
                                  Counts& output,
                                  std::size_t& count) noexcept {
    output = {};
    count = 0;
    if (squadRow >= catalog.squads().size()) {
        return false;
    }
    const auto members = sdk::squad_members(catalog, catalog.squads()[squadRow]);
    if (members.empty() || members.size() > output.size()) {
        return false;
    }
    bool positive = false;
    for (const auto& member : members) {
        if (member.defaultCount < 0) {
            return false;
        }
        output[count++] = member.defaultCount;
        positive = positive || member.defaultCount > 0;
    }
    return positive;
}

/** The physical ClientRef stays the same when its publication route is rebuilt. */
[[nodiscard]] bool same_source(const host::ScriptableTarget& left,
                               const host::ScriptableTarget& right) noexcept {
    return left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotIndex == right.slotIndex && left.slotType == right.slotType
           && left.authSchema == right.authSchema;
}

/** Static scene choices need package rows but no active Host reservation. */
[[nodiscard]] SceneStatus collect(const sdk::BoundView& view,
                                  std::uint32_t occurrenceRow,
                                  std::uint32_t sceneSlotRow,
                                  SceneSpawnPlan& output) noexcept {
    output = {};
    if (view.catalog == nullptr || occurrenceRow >= view.catalog->occurrences().size()) {
        return SceneStatus::invalidView;
    }
    if (view.catalog->occurrences()[occurrenceRow].scenarioIndex != view.scenarioRow) {
        return SceneStatus::wrongScenario;
    }
    if (sceneSlotRow < view.catalog->slots().size()
        && sdk::slot_authored_scene_squad_edges(*view.catalog, view.catalog->slots()[sceneSlotRow])
               .empty()) {
        const state::build_data::scriptables::Snapshot empty{};
        return collect_scene_spawn_plan(*view.catalog, empty, occurrenceRow, sceneSlotRow, output);
    }
    sdk::generated_world::GeneratedWorldView world{};
    if (sdk::generated_world::resolve(view, world) != sdk::generated_world::BindStatus::ready
        || world.snapshot() == nullptr) {
        return SceneStatus::targetUnavailable;
    }
    return collect_scene_spawn_plan(
        *view.catalog, *world.snapshot(), occurrenceRow, sceneSlotRow, output);
}

/** Every target and source policy is checked before a cast can queue its first preparation. */
[[nodiscard]] SceneStatus
prepare_pair(const sdk::BoundView& view, std::uint32_t sceneState, SceneSpawnPair& pair) noexcept {
    devices::detail::PreparedDevice actor{}, source{};
    if (devices::detail::prepare_combatant(view, pair.actorSlotRow, actor) != devices::Status::ready
        || devices::detail::prepare_slot(
               view, view.catalog->squads()[pair.squadRow].slotIndex, source)
               != devices::Status::ready) {
        return SceneStatus::targetUnavailable;
    }
    if (!actor.target.stateLocalRoster || actor.stateRow != sceneState
        || (source.target.stateLocalRoster && source.stateRow != sceneState)) {
        return SceneStatus::wrongState;
    }
    pair.actorTarget = actor.target;
    pair.sourceTarget = source.target;
    const auto actorStatus = host::actor_squad_binding_status(view.binding, pair.actorTarget);
    const auto sourceStatus = host::actor_program_source_status(view.binding, pair.sourceTarget);
    if (actorStatus == host::ActorSquadBindingStatus::incompatible
        || sourceStatus == host::ActorProgramSourceStatus::incompatible) {
        return SceneStatus::refused;
    }
    pair.actorReady = actorStatus == host::ActorSquadBindingStatus::ready;
    pair.sourceReady = sourceStatus == host::ActorProgramSourceStatus::ready;
    if (!pair.sourceReady) {
        Counts counts{};
        std::size_t count = 0;
        if (!default_counts(*view.catalog, pair.squadRow, counts, count)
            || activity_sdk_squads::availability(
                   view, pair.squadRow, std::span(counts).first(count), squad_auth::Mode::reserve)
                   != activity_sdk_squads::Status::ready) {
            return SceneStatus::refused;
        }
    }
    return SceneStatus::ready;
}

} // namespace

/** Sends the cast's squads when the wire can carry them all, otherwise none. */
void scene_dependencies(
    const sdk::Catalog& catalog,
    std::uint32_t sceneSlotRow,
    const SceneSpawnPlan& plan,
    middleware::bap::activity_message::sensor_auth_update::AuthoredSceneDependencies&
        output) noexcept {
    output = {};
    if (plan.count > output.references.size()) {
        std::array<char, core::log::kLineCapacity> line{};
        const std::string_view scene = sceneSlotRow < catalog.slots().size()
                                           ? catalog.string(catalog.slots()[sceneSlotRow].id)
                                           : std::string_view{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=scene_dependencies result=omitted scene=%.*s "
                                          "cast=%zu capacity=%zu",
                                          static_cast<int>(scene.size()),
                                          scene.data(),
                                          plan.count,
                                          output.references.size());
        if (written > 0) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::warn,
                {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
        }
        return;
    }
    for (std::size_t index = 0; index < plan.count; ++index) {
        const host::ScriptableTarget& source = plan.pairs[index].sourceTarget;
        output.references[output.count++] = {source.registryKey,
                                             static_cast<std::int8_t>(source.slotType),
                                             static_cast<std::int16_t>(source.slotIndex)};
    }
}

/** Returns immutable scene cast identities without consulting live output state. */
SceneStatus resolve_scene_spawn_plan(const sdk::BoundView& view,
                                     std::uint32_t occurrenceRow,
                                     std::uint32_t sceneSlotRow,
                                     SceneSpawnPlan& output) noexcept {
    return collect(view, occurrenceRow, sceneSlotRow, output);
}

/**
 * Captures the exact parent rows without touching Host state.
 * @param view Pinned SDK and generated world.
 * @param occurrenceRow Selected scene occurrence.
 * @param sceneSlotRow Selected Type 43 slot.
 * @param output Receives source squad rows; cleared on failure.
 * @param count Receives the written row count; zero on failure.
 * @return Ready for a complete bounded cast, including an empty cast.
 */
SceneStatus scene_spawn_sources(const sdk::BoundView& view,
                                std::uint32_t occurrenceRow,
                                std::uint32_t sceneSlotRow,
                                std::span<std::uint32_t> output,
                                std::size_t& count) noexcept {
    std::fill(output.begin(), output.end(), sdk::format::kAbsentIndex);
    count = 0;
    SceneSpawnPlan plan{};
    const auto status = collect(view, occurrenceRow, sceneSlotRow, plan);
    if (status != SceneStatus::ready) {
        return status;
    }
    if (plan.count > output.size()) {
        return SceneStatus::refused;
    }
    for (std::size_t index = 0; index < plan.count; ++index) {
        output[count++] = plan.pairs[index].squadRow;
    }
    return SceneStatus::ready;
}

/**
 * Resolves every cast member and refuses incompatible live ownership before any output.
 * @param view Current SDK and activity binding.
 * @param occurrenceRow Selected scene occurrence.
 * @param sceneSlotRow Selected Type 43 slot.
 * @param output Receives the complete current plan; cleared on failure.
 * @return Ready only when every source can be reused or prepared.
 */
SceneStatus query_scene_spawn_plan(const sdk::BoundView& view,
                                   std::uint32_t occurrenceRow,
                                   std::uint32_t sceneSlotRow,
                                   SceneSpawnPlan& output) noexcept {
    output = {};
    detail::PreparedScene scene{};
    const auto available = detail::prepare_scene(view, occurrenceRow, sceneSlotRow, scene);
    if (available != SceneStatus::ready) {
        return available;
    }
    SceneSpawnPlan candidate{};
    const auto collected = collect(view, occurrenceRow, sceneSlotRow, candidate);
    if (collected != SceneStatus::ready) {
        return collected;
    }
    for (std::size_t index = 0; index < candidate.count; ++index) {
        const auto status = prepare_pair(view, scene.stateRow, candidate.pairs[index]);
        if (status != SceneStatus::ready) {
            return status;
        }
    }
    output = candidate;
    return SceneStatus::ready;
}

/**
 * Reuses retained preparation and queues only the next missing world change.
 * @param view Current SDK and activity binding.
 * @param occurrenceRow Selected scene occurrence.
 * @param sceneSlotRow Selected Type 43 slot.
 * @param reservation Exact output revision owned by the durable scene intent.
 * @return Queued for one preparation or the final scene activation.
 */
SceneStatus activate_authored_scene_spawn_reserved(
    const sdk::BoundView& view,
    std::uint32_t occurrenceRow,
    std::uint32_t sceneSlotRow,
    const host::ScriptableOutputReservation& reservation) noexcept {
    SceneSpawnPlan plan{};
    const auto status = query_scene_spawn_plan(view, occurrenceRow, sceneSlotRow, plan);
    if (status != SceneStatus::ready) {
        return status;
    }
    for (std::size_t index = 0; index < plan.count; ++index) {
        const auto& pair = plan.pairs[index];
        if (!pair.sourceReady) {
            Counts counts{};
            std::size_t count = 0;
            if (!default_counts(*view.catalog, pair.squadRow, counts, count)) {
                return SceneStatus::refused;
            }
            return activity_sdk_squads::place_reserved(view,
                                                       pair.squadRow,
                                                       std::span(counts).first(count),
                                                       squad_auth::Mode::reserve,
                                                       reservation)
                           == activity_sdk_squads::Status::queued
                       ? SceneStatus::queued
                       : SceneStatus::refused;
        }
        if (!pair.actorReady) {
            return devices::bind_combatant_to_squad_reserved(view, pair.actorSlotRow, reservation)
                           == devices::Status::queued
                       ? SceneStatus::queued
                       : SceneStatus::refused;
        }
    }
    return activate_authored_scene_reserved(view, occurrenceRow, sceneSlotRow, reservation);
}

/**
 * A transported preparation must belong to this scene's exact static cast.
 * @param view Current SDK and activity binding.
 * @param occurrenceRow Selected scene occurrence.
 * @param sceneSlotRow Selected Type 43 slot.
 * @param staged Exact transported Host output.
 * @param squadRow Receives the preparation's source squad row; absent on failure.
 * @return Ready only for an owned source reservation or actor binding.
 */
SceneStatus validate_scene_preparation_output(const sdk::BoundView& view,
                                              std::uint32_t occurrenceRow,
                                              std::uint32_t sceneSlotRow,
                                              const host::PendingScriptableOverride& staged,
                                              std::uint32_t& squadRow) noexcept {
    squadRow = sdk::format::kAbsentIndex;
    if (staged.kind != host::ScriptableOverrideKind::squad
        && staged.kind != host::ScriptableOverrideKind::combatantBinding) {
        return SceneStatus::refused;
    }
    SceneSpawnPlan plan{};
    const auto status = collect(view, occurrenceRow, sceneSlotRow, plan);
    if (status != SceneStatus::ready) {
        return status;
    }
    for (std::size_t index = 0; index < plan.count; ++index) {
        const auto& pair = plan.pairs[index];
        const auto& target = staged.kind == host::ScriptableOverrideKind::squad ? pair.sourceTarget
                                                                                : pair.actorTarget;
        if (same_source(staged.target, target)) {
            squadRow = pair.squadRow;
            return SceneStatus::ready;
        }
    }
    return SceneStatus::targetUnavailable;
}

} // namespace sunrise::server::activity::activity_sdk_mission
