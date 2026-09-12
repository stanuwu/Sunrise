#include <Windows.h>

#include <array>
#include <limits>

#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../state/activity/runtime.h"
#include "activity_sdk_behavior_scope.h"
#include "activity_sdk_mission_internal.h"
#include "activity_sdk_scene_dependencies.h"
#include "activity_sdk_scriptable_route.h"

namespace sunrise::server::activity::activity_sdk_mission::detail {

namespace layouts = state::build_data::scenarios;
namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace sdk = state::activity_sdk;
namespace sdk_route = server::activity::activity_sdk_scriptables;

namespace {

SRWLOCK g_materializeLock{SRWLOCK_INIT};
std::array<layouts::RosterGroup, message::kPublishedGroupCapacity> g_materializedGroups{};

/** Maps the SDK materializer's closed set of semantic failures. */
[[nodiscard]] Status mission_status(sdk::MissionSeedStatus status) noexcept {
    switch (status) {
    case sdk::MissionSeedStatus::ready:
        return Status::ready;
    case sdk::MissionSeedStatus::invalidView:
        return Status::invalidView;
    case sdk::MissionSeedStatus::invalidSliceSet:
        return Status::missingLiveSliceSet;
    case sdk::MissionSeedStatus::missingInitialState:
        return Status::missingInitialState;
    case sdk::MissionSeedStatus::ambiguousInitialState:
        return Status::ambiguousInitialState;
    case sdk::MissionSeedStatus::invalidOccurrence:
        return Status::invalidOccurrence;
    case sdk::MissionSeedStatus::schemaJoinNotExact:
        return Status::schemaJoinNotExact;
    case sdk::MissionSeedStatus::invalidRosterGroup:
        return Status::invalidRosterGroup;
    case sdk::MissionSeedStatus::rosterKeyConflict:
        return Status::rosterKeyConflict;
    case sdk::MissionSeedStatus::groupCapacityExceeded:
        return Status::groupCapacityExceeded;
    }
    return Status::refused;
}

/** @return True when two plans leave the same objects out of their seed, in the same order. */
[[nodiscard]] bool same_omissions(const server::bap::ActivityMissionSeedPlan& left,
                                  const server::bap::ActivityMissionSeedPlan& right) noexcept {
    if (left.omissionCount != right.omissionCount) {
        return false;
    }
    for (std::uint32_t index = 0; index < left.omissionCount; ++index) {
        if (left.omissions[index].objectTag != right.omissions[index].objectTag
            || left.omissions[index].registryKey != right.omissions[index].registryKey) {
            return false;
        }
    }
    return true;
}

/** Copies the immutable scalar materializer output into a transport-owned plan. */
[[nodiscard]] server::bap::ActivityMissionSeedPlan
plan_from(const sdk::MissionSeedSummary& summary) noexcept {
    server::bap::ActivityMissionSeedPlan plan{};
    plan.activityRow = summary.activityRow;
    plan.scenarioRow = summary.scenarioRow;
    plan.stateRow = summary.stateRow;
    plan.bubbleRow = summary.bubbleRow;
    plan.bubbleOrdinal = summary.bubbleOrdinal;
    plan.stateOrdinal = summary.stateOrdinal;
    plan.entryIndex = summary.entryIndex;
    plan.sliceSetIndex = summary.sliceSetIndex;
    plan.effectiveRegion = summary.effectiveRegion;
    plan.omissions = summary.omissions;
    plan.omissionCount = summary.omissionCount;
    plan.occurrenceCount = summary.occurrenceCount;
    plan.groupCount = summary.groupCount;
    plan.authMappingSlots = summary.authMappingSlots;
    plan.authResetSlots = summary.authResetSlots;
    plan.senseSuppressedSlots = summary.senseSuppressedSlots;
    return plan;
}

} // namespace

/** @return True when two plans name the same immutable generated rows and counts. */
[[nodiscard]] bool same_plan(const server::bap::ActivityMissionSeedPlan& left,
                             const server::bap::ActivityMissionSeedPlan& right) noexcept {
    return left.activityRow == right.activityRow && left.scenarioRow == right.scenarioRow
           && left.stateRow == right.stateRow && left.bubbleRow == right.bubbleRow
           && left.bubbleOrdinal == right.bubbleOrdinal && left.stateOrdinal == right.stateOrdinal
           && left.entryIndex == right.entryIndex && left.sliceSetIndex == right.sliceSetIndex
           && left.effectiveRegion == right.effectiveRegion && same_omissions(left, right)
           && left.occurrenceCount == right.occurrenceCount && left.groupCount == right.groupCount
           && left.authMappingSlots == right.authMappingSlots
           && left.authResetSlots == right.authResetSlots
           && left.senseSuppressedSlots == right.senseSuppressedSlots;
}

namespace {

/** Materializes into static lock-owned storage so UI stack size stays bounded. */

/** Checks the exact enabled and published mission-seed lease for one scene state. */
[[nodiscard]] SceneStatus scene_lease_status(const sdk::BoundView& view,
                                             const server::bap::ActivityLinkView& link,
                                             std::uint32_t stateRow) noexcept {
    server::bap::ActivityMissionSeedLeaseView lease{};
    switch (server::bap::activity_mission_seed_lease(
        view.binding, view.scenarioRow, link.activityClientGeneration, lease)) {
    case server::bap::ActivityMissionSeedLeaseStatus::ready:
        break;
    case server::bap::ActivityMissionSeedLeaseStatus::noActivityLink:
        return SceneStatus::noActivityLink;
    case server::bap::ActivityMissionSeedLeaseStatus::staleActivityClient:
        return SceneStatus::staleActivityClient;
    case server::bap::ActivityMissionSeedLeaseStatus::outputBusy:
        return SceneStatus::outputBusy;
    case server::bap::ActivityMissionSeedLeaseStatus::wrongScenario:
    case server::bap::ActivityMissionSeedLeaseStatus::missingLiveSliceSet:
    case server::bap::ActivityMissionSeedLeaseStatus::wrongSliceSet:
    case server::bap::ActivityMissionSeedLeaseStatus::refused:
        return SceneStatus::missionSeedUnavailable;
    }
    if (lease.activityClientGeneration != link.activityClientGeneration) {
        return SceneStatus::staleActivityClient;
    }
    if (!lease.configured || lease.revision == 0 || lease.plan.scenarioRow != view.scenarioRow) {
        return SceneStatus::missionSeedUnavailable;
    }
    if (lease.plan.stateRow != stateRow
        && !behavior_scope::live_state(view.catalog->states(),
                                       view.catalog->bubbles(),
                                       view.scenarioRow,
                                       stateRow,
                                       link.effectiveRegion)) {
        return SceneStatus::wrongState;
    }
    if (lease.publicationPending || lease.publishedRevision != lease.revision) {
        return SceneStatus::missionSeedPending;
    }
    return SceneStatus::ready;
}

} // namespace

/** Maps the shared binding result to the authored-scene refusal surface. */
[[nodiscard]] SceneStatus scene_binding_status(const sdk::BoundView& view,
                                               server::bap::ActivityLinkView& link) noexcept {
    switch (binding_status(view, link)) {
    case Status::ready:
        // A public-target link publishes the state-local groups of the public bubbles it hosts,
        // so a scene, task or cue in one of them is reachable only through that link.
        return link.effectiveRegion >= 0 ? SceneStatus::ready : SceneStatus::noActivityLink;
    case Status::staleBinding:
        return SceneStatus::staleBinding;
    case Status::staleActivityClient:
        return SceneStatus::staleActivityClient;
    case Status::noActivityLink:
        return SceneStatus::noActivityLink;
    case Status::invalidView:
    case Status::missingLiveSliceSet:
    case Status::wrongScenario:
    case Status::wrongSliceSet:
    case Status::missingInitialState:
    case Status::ambiguousInitialState:
    case Status::invalidOccurrence:
    case Status::schemaJoinNotExact:
    case Status::invalidRosterGroup:
    case Status::rosterKeyConflict:
    case Status::groupCapacityExceeded:
    case Status::outputBusy:
    case Status::refused:
        return SceneStatus::invalidView;
    }
    return SceneStatus::invalidView;
}

/** Maps exact SDK binding validation to this facade's stable refusal surface. */
[[nodiscard]] Status binding_status(const sdk::BoundView& view,
                                    server::bap::ActivityLinkView& link) noexcept {
    if (view.catalog == nullptr || sdk::bound_activity(view) == nullptr
        || sdk::bound_scenario(view) == nullptr) {
        return Status::invalidView;
    }
    if (!state::activity::binding_matches(view.binding)) {
        return Status::staleBinding;
    }
    (void)server::bap::activity_link_view(view.binding, link);
    switch (
        sdk::revalidate(view, view.binding, link.matchingLinks, link.activityClientGeneration)) {
    case sdk::Status::ready:
        return Status::ready;
    case sdk::Status::missingClient:
    case sdk::Status::ambiguousClient:
        return Status::noActivityLink;
    case sdk::Status::staleSession:
        return Status::staleBinding;
    case sdk::Status::staleActivityClient:
        return Status::staleActivityClient;
    case sdk::Status::notReady:
    case sdk::Status::missing:
    case sdk::Status::wrongSdkBuild:
    case sdk::Status::catalogInvalid:
    case sdk::Status::wrongActivity:
    case sdk::Status::activityJoinNotExact:
    case sdk::Status::missingScenarioLink:
        return Status::invalidView;
    }
    return Status::invalidView;
}

/** Maps the transport lease's closed set of connection/refusal outcomes. */
[[nodiscard]] Status lease_status(server::bap::ActivityMissionSeedLeaseStatus status) noexcept {
    using LeaseStatus = server::bap::ActivityMissionSeedLeaseStatus;
    switch (status) {
    case LeaseStatus::ready:
        return Status::ready;
    case LeaseStatus::noActivityLink:
        return Status::noActivityLink;
    case LeaseStatus::staleActivityClient:
        return Status::staleActivityClient;
    case LeaseStatus::wrongScenario:
        return Status::wrongScenario;
    case LeaseStatus::missingLiveSliceSet:
        return Status::missingLiveSliceSet;
    case LeaseStatus::wrongSliceSet:
        return Status::wrongSliceSet;
    case LeaseStatus::outputBusy:
        return Status::outputBusy;
    case LeaseStatus::refused:
        return Status::refused;
    }
    return Status::refused;
}

/**
 * Materializes into static lock-owned storage so UI stack size stays bounded.
 * @param omissions Objects the caller leaves out of the seed.
 * @param output Cleared, then filled only when the materializer reports ready.
 */
[[nodiscard]] Status materialize_plan(const sdk::BoundView& view,
                                      std::int32_t effectiveRegion,
                                      std::span<const sdk::MissionSeedOmission> omissions,
                                      server::bap::ActivityMissionSeedPlan& output) noexcept {
    output = {};
    if (effectiveRegion < 0) {
        return Status::missingLiveSliceSet;
    }
    sdk::MissionSeedSummary summary{};
    AcquireSRWLockExclusive(&g_materializeLock);
    const sdk::MissionSeedStatus materialized = sdk::materialize_initial_mission_seed(
        view, effectiveRegion, omissions, std::span(g_materializedGroups), summary);
    ReleaseSRWLockExclusive(&g_materializeLock);
    const Status status = mission_status(materialized);
    if (status == Status::ready) {
        output = plan_from(summary);
    }
    return status;
}

/** Reads the transport lease after the binding has been revalidated. */
[[nodiscard]] Status read_lease(const sdk::BoundView& view,
                                const server::bap::ActivityLinkView& link,
                                server::bap::ActivityMissionSeedLeaseView& output) noexcept {
    return lease_status(server::bap::activity_mission_seed_lease(
        view.binding, view.scenarioRow, link.activityClientGeneration, output));
}

/** Resolves one exact generated scene without changing transport state. */
[[nodiscard]] SceneStatus prepare_scene(const sdk::BoundView& view,
                                        std::uint32_t occurrenceRow,
                                        std::uint32_t slotRow,
                                        PreparedScene& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const SceneStatus live = scene_binding_status(view, link);
    if (live != SceneStatus::ready) {
        return live;
    }

    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    if (scenario == nullptr || occurrenceRow >= occurrences.size()) {
        return SceneStatus::invalidOccurrence;
    }
    if (slotRow >= slots.size()) {
        return SceneStatus::invalidSlot;
    }

    const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
    if (occurrence.scenarioIndex != view.scenarioRow) {
        return SceneStatus::wrongScenario;
    }
    if (occurrence.objectIndex >= objects.size() || occurrence.stateIndex >= states.size()
        || occurrence.bubbleIndex >= bubbles.size()) {
        return SceneStatus::invalidOccurrence;
    }
    const sdk::format::Object& object = objects[occurrence.objectIndex];
    const sdk::format::Slot& slot = slots[slotRow];
    const sdk::format::State& state = states[occurrence.stateIndex];
    const sdk::format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
    if (slot.objectIndex != occurrence.objectIndex || slotRow < object.slots.first
        || slotRow - object.slots.first >= object.slots.count) {
        return SceneStatus::invalidSlot;
    }
    if (state.scenarioIndex != view.scenarioRow || bubble.scenarioIndex != view.scenarioRow
        || state.bubbleIndex != occurrence.bubbleIndex) {
        return SceneStatus::invalidOccurrence;
    }
    if (slot.slotType != sdk::format::kAuthoredSceneSlotType
        || slot.componentClass != sdk::format::kAuthoredSceneComponentClass
        || slot.senseSchema != sdk::format::kAuthoredSceneSenseSchema
        || slot.authSchema != sdk::format::kAuthoredSceneAuthSchema) {
        return SceneStatus::invalidSlot;
    }
    if ((slot.flags & sdk::format::kSlotSchemaJoinExact) == 0) {
        return SceneStatus::schemaJoinNotExact;
    }
    if (slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()
        || slot.slotType > (std::numeric_limits<std::uint8_t>::max)()) {
        return SceneStatus::invalidSlot;
    }

    const auto resources = sdk::slot_authored_scene_resources(catalog, slot);
    if (resources.empty()) {
        return SceneStatus::missingResource;
    }
    if (resources.size() != 1) {
        return SceneStatus::ambiguousResource;
    }
    const sdk::format::AuthoredSceneResource& resource = resources.front();
    if (resource.slotIndex != slotRow || resource.configTag == 0
        || resource.configTag == sdk::format::kAbsentIndex || resource.resourceTag == 0
        || resource.resourceTag == sdk::format::kAbsentIndex
        || resource.resourceClass != sdk::format::kAuthoredSceneResourceClass
        || resource.flags != sdk::format::kAuthoredSceneResourceExact
        || resource.descriptorOffset > (std::numeric_limits<std::uint32_t>::max)()
                                           - sdk::format::kAuthoredSceneResourceRelativeOffset
        || resource.resourceFieldOffset
               != resource.descriptorOffset + sdk::format::kAuthoredSceneResourceRelativeOffset) {
        return SceneStatus::missingResource;
    }

    const SceneStatus dependencies =
        scene_dependencies(catalog, slot, resource, output.sceneDependencies);
    if (dependencies != SceneStatus::ready) {
        return dependencies;
    }

    const SceneStatus lease = scene_lease_status(view, link, occurrence.stateIndex);
    if (lease != SceneStatus::ready) {
        return lease;
    }

    sdk_route::SourceIdentity source{};
    source.scenarioTag = scenario->tag;
    source.objectTag = object.objectTag;
    source.registryKey = object.objectKey;
    source.authSchema = slot.authSchema;
    source.objectRow = occurrence.objectIndex;
    source.stateRow = occurrence.stateIndex;
    source.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    source.slotType = static_cast<std::uint8_t>(slot.slotType);

    sdk_route::Resolution resolved{};
    switch (sdk_route::resolve(view, source, link.effectiveRegion, resolved)) {
    case sdk_route::Status::ready:
        break;
    case sdk_route::Status::invalidView:
        return SceneStatus::invalidView;
    case sdk_route::Status::invalidSource:
        return SceneStatus::invalidSlot;
    case sdk_route::Status::missingSource:
        return SceneStatus::targetUnavailable;
    case sdk_route::Status::ambiguousSource:
        return SceneStatus::ambiguousTarget;
    }
    if (resolved.scenarioRow != view.scenarioRow || resolved.stateRow != occurrence.stateIndex
        || resolved.region != link.effectiveRegion || !resolved.target.stateLocalRoster
        || resolved.target.slotType != sdk::format::kAuthoredSceneSlotType
        || resolved.target.authSchema != sdk::format::kAuthoredSceneAuthSchema) {
        return SceneStatus::invalidSlot;
    }

    output.target = resolved.target;
    output.rosterGroup = resolved.rosterGroup;
    output.activityClientGeneration = link.activityClientGeneration;
    output.scenarioRow = resolved.scenarioRow;
    output.stateRow = resolved.stateRow;
    output.effectiveRegion = resolved.region;
    return SceneStatus::ready;
}

/** Resolves one exact SDK-bounded type-53 cue without changing transport state. */
[[nodiscard]] SceneStatus prepare_dialogue(const sdk::BoundView& view,
                                           std::uint32_t occurrenceRow,
                                           std::uint32_t slotRow,
                                           std::uint16_t cueIndex,
                                           PreparedScene& output,
                                           std::uint16_t& authoredCueCount) noexcept {
    output = {};
    authoredCueCount = 0;
    server::bap::ActivityLinkView link{};
    const SceneStatus live = scene_binding_status(view, link);
    if (live != SceneStatus::ready) {
        return live;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    if (scenario == nullptr || occurrenceRow >= occurrences.size()) {
        return SceneStatus::invalidOccurrence;
    }
    if (slotRow >= slots.size()) {
        return SceneStatus::invalidSlot;
    }
    const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
    if (occurrence.scenarioIndex != view.scenarioRow || occurrence.objectIndex >= objects.size()
        || occurrence.stateIndex >= states.size() || occurrence.bubbleIndex >= bubbles.size()) {
        return occurrence.scenarioIndex != view.scenarioRow ? SceneStatus::wrongScenario
                                                            : SceneStatus::invalidOccurrence;
    }
    const sdk::format::Object& object = objects[occurrence.objectIndex];
    const sdk::format::Slot& slot = slots[slotRow];
    const sdk::format::State& state = states[occurrence.stateIndex];
    const sdk::format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
    if (slot.objectIndex != occurrence.objectIndex || slotRow < object.slots.first
        || slotRow - object.slots.first >= object.slots.count
        || state.scenarioIndex != view.scenarioRow || bubble.scenarioIndex != view.scenarioRow
        || state.bubbleIndex != occurrence.bubbleIndex) {
        return SceneStatus::invalidSlot;
    }
    if (slot.slotType != sdk::format::kDialogueSlotType
        || slot.componentClass != sdk::format::kDialogueComponentClass
        || slot.authSchema != sdk::format::kDialogueAuthSchema
        || (slot.flags & (sdk::format::kSlotSchemaJoinExact | sdk::format::kSlotDialogueCuesExact))
               != (sdk::format::kSlotSchemaJoinExact | sdk::format::kSlotDialogueCuesExact)
        || slot.reserved == 0 || slot.reserved > sdk::format::kDialogueMaximumCueCount
        || cueIndex >= slot.reserved) {
        return SceneStatus::invalidSlot;
    }
    authoredCueCount = static_cast<std::uint16_t>(slot.reserved);
    const SceneStatus lease = scene_lease_status(view, link, occurrence.stateIndex);
    if (lease != SceneStatus::ready) {
        return lease;
    }
    sdk_route::SourceIdentity source{};
    source.scenarioTag = scenario->tag;
    source.objectTag = object.objectTag;
    source.registryKey = object.objectKey;
    source.authSchema = slot.authSchema;
    source.objectRow = occurrence.objectIndex;
    source.stateRow = occurrence.stateIndex;
    source.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    source.slotType = static_cast<std::uint8_t>(slot.slotType);
    sdk_route::Resolution resolved{};
    switch (sdk_route::resolve(view, source, link.effectiveRegion, resolved)) {
    case sdk_route::Status::ready:
        break;
    case sdk_route::Status::invalidView:
        return SceneStatus::invalidView;
    case sdk_route::Status::invalidSource:
        return SceneStatus::invalidSlot;
    case sdk_route::Status::missingSource:
        return SceneStatus::targetUnavailable;
    case sdk_route::Status::ambiguousSource:
        return SceneStatus::ambiguousTarget;
    }
    if (resolved.scenarioRow != view.scenarioRow || resolved.stateRow != occurrence.stateIndex
        || resolved.region != link.effectiveRegion || !resolved.target.stateLocalRoster
        || resolved.target.slotType != sdk::format::kDialogueSlotType
        || resolved.target.authSchema != sdk::format::kDialogueAuthSchema) {
        return SceneStatus::invalidSlot;
    }
    output.target = resolved.target;
    output.rosterGroup = resolved.rosterGroup;
    output.activityClientGeneration = link.activityClientGeneration;
    output.scenarioRow = resolved.scenarioRow;
    output.stateRow = resolved.stateRow;
    output.effectiveRegion = resolved.region;
    return SceneStatus::ready;
}

/** Resolves one exact fixed-schema behavior slot without changing transport state. */
[[nodiscard]] SceneStatus prepare_typed_behavior(const sdk::BoundView& view,
                                                 std::uint32_t occurrenceRow,
                                                 std::uint32_t slotRow,
                                                 std::uint32_t expectedSlotType,
                                                 std::uint32_t expectedComponentClass,
                                                 std::uint32_t expectedAuthSchema,
                                                 bool requireTaskTarget,
                                                 PreparedScene& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const SceneStatus live = scene_binding_status(view, link);
    if (live != SceneStatus::ready) {
        return live;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const sdk::format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    const auto states = catalog.states();
    const auto bubbles = catalog.bubbles();
    if (scenario == nullptr || occurrenceRow >= occurrences.size()) {
        return SceneStatus::invalidOccurrence;
    }
    if (slotRow >= slots.size()) {
        return SceneStatus::invalidSlot;
    }
    const sdk::format::Occurrence& occurrence = occurrences[occurrenceRow];
    if (occurrence.scenarioIndex != view.scenarioRow || occurrence.objectIndex >= objects.size()
        || occurrence.stateIndex >= states.size() || occurrence.bubbleIndex >= bubbles.size()) {
        return occurrence.scenarioIndex != view.scenarioRow ? SceneStatus::wrongScenario
                                                            : SceneStatus::invalidOccurrence;
    }
    const sdk::format::Object& object = objects[occurrence.objectIndex];
    const sdk::format::Slot& slot = slots[slotRow];
    const sdk::format::State& state = states[occurrence.stateIndex];
    const sdk::format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
    if (slot.objectIndex != occurrence.objectIndex || slotRow < object.slots.first
        || slotRow - object.slots.first >= object.slots.count
        || state.scenarioIndex != view.scenarioRow || bubble.scenarioIndex != view.scenarioRow
        || state.bubbleIndex != occurrence.bubbleIndex) {
        return SceneStatus::invalidSlot;
    }
    if (slot.slotType != expectedSlotType || slot.componentClass != expectedComponentClass
        || slot.authSchema != expectedAuthSchema
        || (slot.flags & sdk::format::kSlotSchemaJoinExact) == 0
        || (requireTaskTarget && sdk::slot_task_targets(catalog, slot).empty())
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()) {
        return SceneStatus::invalidSlot;
    }
    const SceneStatus lease = scene_lease_status(view, link, occurrence.stateIndex);
    if (lease != SceneStatus::ready) {
        return lease;
    }
    sdk_route::SourceIdentity source{};
    source.scenarioTag = scenario->tag;
    source.objectTag = object.objectTag;
    source.registryKey = object.objectKey;
    source.authSchema = slot.authSchema;
    source.objectRow = occurrence.objectIndex;
    source.stateRow = occurrence.stateIndex;
    source.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    source.slotType = static_cast<std::uint8_t>(slot.slotType);
    sdk_route::Resolution resolved{};
    switch (sdk_route::resolve(view, source, link.effectiveRegion, resolved)) {
    case sdk_route::Status::ready:
        break;
    case sdk_route::Status::invalidView:
        return SceneStatus::invalidView;
    case sdk_route::Status::invalidSource:
        return SceneStatus::invalidSlot;
    case sdk_route::Status::missingSource:
        return SceneStatus::targetUnavailable;
    case sdk_route::Status::ambiguousSource:
        return SceneStatus::ambiguousTarget;
    }
    if (resolved.scenarioRow != view.scenarioRow || resolved.stateRow != occurrence.stateIndex
        || resolved.region != link.effectiveRegion || !resolved.target.stateLocalRoster
        || resolved.target.slotType != expectedSlotType
        || resolved.target.authSchema != expectedAuthSchema) {
        return SceneStatus::invalidSlot;
    }
    output.target = resolved.target;
    output.rosterGroup = resolved.rosterGroup;
    output.activityClientGeneration = link.activityClientGeneration;
    output.scenarioRow = resolved.scenarioRow;
    output.stateRow = resolved.stateRow;
    output.effectiveRegion = resolved.region;
    return SceneStatus::ready;
}

/** Resolves one exact SDK-linked type-38 task without changing transport state. */
[[nodiscard]] SceneStatus prepare_task(const sdk::BoundView& view,
                                       std::uint32_t occurrenceRow,
                                       std::uint32_t slotRow,
                                       PreparedScene& output) noexcept {
    return prepare_typed_behavior(view,
                                  occurrenceRow,
                                  slotRow,
                                  sdk::format::kTaskSlotType,
                                  sdk::format::kTaskComponentClass,
                                  sdk::format::kTaskAuthSchema,
                                  true,
                                  output);
}

/** Resolves one exact type-68 HUD state without changing transport state. */
[[nodiscard]] SceneStatus prepare_directive(const sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow,
                                            std::uint32_t nameHash,
                                            std::int32_t elementIndex,
                                            PreparedScene& output) noexcept {
    const SceneStatus status = prepare_typed_behavior(view,
                                                      occurrenceRow,
                                                      slotRow,
                                                      sdk::format::kDirectiveSlotType,
                                                      sdk::format::kDirectiveComponentClass,
                                                      sdk::format::kDirectiveAuthSchema,
                                                      false,
                                                      output);
    if (status != SceneStatus::ready) {
        return status;
    }
    std::size_t matches = 0;
    for (const sdk::format::DirectiveElement& row : view.catalog->directive_elements()) {
        matches +=
            row.slotIndex == slotRow && row.nameHash == nameHash && row.elementIndex == elementIndex
                ? 1U
                : 0U;
    }
    return matches == 1 ? SceneStatus::ready : SceneStatus::invalidSlot;
}

/** Resolves one exact type-3 objective sensor without changing transport state. */
[[nodiscard]] SceneStatus prepare_objective(const sdk::BoundView& view,
                                            std::uint32_t occurrenceRow,
                                            std::uint32_t slotRow,
                                            PreparedScene& output) noexcept {
    return prepare_typed_behavior(view,
                                  occurrenceRow,
                                  slotRow,
                                  sdk::format::kObjectiveSlotType,
                                  sdk::format::kObjectiveComponentClass,
                                  sdk::format::kObjectiveAuthSchema,
                                  false,
                                  output);
}

} // namespace sunrise::server::activity::activity_sdk_mission::detail
