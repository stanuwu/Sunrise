#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include "../../core/filesystem/path.h"
#include "../../middleware/bap/activity_message/wire_schema/activity_communication_route.h"
#include "../activity/definition.h"
#include "../build_data/scenarios/definition.h"
#include "format.h"
#include "identity.h"

namespace sunrise::state::activity_sdk {

/** Catalog-authorized identity required before a runtime SDK pack may be mapped. */
using ExpectedIdentity = identity::Expected;

/** Load and binding outcomes are stable panel-facing refusal names. */
enum class Status : std::uint8_t {
    notReady,
    ready,
    missing,
    wrongSdkBuild,
    catalogInvalid,
    missingClient,
    ambiguousClient,
    staleSession,
    wrongActivity,
    activityJoinNotExact,
    missingScenarioLink,
    staleActivityClient,
};

/** Fail-closed outcomes from building one activity's initial state-local mission plan. */
enum class MissionSeedStatus : std::uint8_t {
    ready,
    invalidView,
    invalidSliceSet,
    missingInitialState,
    ambiguousInitialState,
    invalidOccurrence,
    schemaJoinNotExact,
    invalidRosterGroup,
    rosterKeyConflict,
    groupCapacityExceeded,
};

/** Fail-closed outcomes from materializing one object's descriptor-backed type-43 seeds. */
enum class AuthoredSceneSeedStatus : std::uint8_t {
    ready,
    invalidObject,
    schemaMismatch,
    missingResource,
    ambiguousResource,
    capacityExceeded,
};

/** One exact SDK-owned authored-scene input, without assigning its resource wire semantics. */
struct AuthoredSceneSeed final {
    std::uint32_t objectTag;
    std::uint32_t registryKey;
    std::uint32_t slotIndex;
    std::uint32_t slotType;
    std::uint32_t authSchema;
    std::uint32_t resourceTag;
};

/** Six packed 32-bit fields; the size pins that layout against ABI drift. */
inline constexpr std::size_t kAuthoredSceneSeedSize = 24;

static_assert(std::is_trivial_v<AuthoredSceneSeed> && std::is_standard_layout_v<AuthoredSceneSeed>);
static_assert(sizeof(AuthoredSceneSeed) == kAuthoredSceneSeedSize);

/** The mission owns which objects its seed leaves out, so Mission State owns the type. */
using MissionSeedOmission = ::sunrise::state::activity::mission::MissionSeedOmission;
inline constexpr std::size_t kMissionSeedOmitCapacity =
    ::sunrise::state::activity::mission::kMissionSeedOmitCapacity;

/** Scalar identity and msg-5 work counts for one materialized initial mission plan. */
struct MissionSeedSummary final {
    std::uint32_t activityRow{format::kAbsentIndex};
    std::uint32_t scenarioRow{format::kAbsentIndex};
    /** Catalog-global state row, not its ordinal inside the bubble. */
    std::uint32_t stateRow{format::kAbsentIndex};
    /** Catalog-global bubble row and its authored ordinal. */
    std::uint32_t bubbleRow{format::kAbsentIndex};
    std::uint32_t bubbleOrdinal{format::kAbsentIndex};
    std::uint32_t stateOrdinal{format::kAbsentIndex};
    /** Exact package entry and full slice-set identities stored on the selected state. */
    std::uint32_t entryIndex{format::kAbsentIndex};
    std::uint32_t sliceSetIndex{format::kAbsentIndex};
    /** Exact authored membership region: sliceSetIndex + stateOrdinal. */
    std::uint32_t effectiveRegion{format::kAbsentIndex};
    /** Exact-state occurrence rows visited, including rows with no descriptor-backed slots. */
    std::uint32_t occurrenceCount{};
    /** Unique state-local roster groups written to the caller's output span. */
    std::uint32_t groupCount{};
    /** Descriptor-backed objects left out because the game replicates none of their placements. */
    std::uint32_t unreplicatedObjectCount{};
    /** Objects left out because a declared slot has no descriptor, which would stall the seed. */
    std::uint32_t incompleteObjectCount{};
    /** Objects the mission asked to leave out, echoed so a plan carries what produced it. */
    std::array<MissionSeedOmission, kMissionSeedOmitCapacity> omissions{};
    std::uint32_t omissionCount{};
    /** Descriptor-backed slots contributed by those unique groups. */
    std::uint32_t authMappingSlots{};
    /** Contributed slots whose initial msg-5 block carries an Auth reset. */
    std::uint32_t authResetSlots{};
    /** Contributed slots with a Sense schema whose initial sense-present bit is suppressed. */
    std::uint32_t senseSuppressedSlots{};
};

/** One validated memory mapping owns every borrowed row and string view. */
class Catalog final {
public:
    Catalog() noexcept = default;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    ~Catalog() noexcept;

    /** @return The generator build digest pinned by this mapping. */
    [[nodiscard]] std::span<const std::byte> sdk_build_sha256() const noexcept;
    /** @return The exact authenticated payload digest of this mapped pack. */
    [[nodiscard]] std::span<const std::byte> payload_sha256() const noexcept;
    /** @return The source-content digest carried by this mapping. */
    [[nodiscard]] std::span<const std::byte> content_key_sha256() const noexcept;
    /** @return The normalized logical-IR digest carried by this mapping. */
    [[nodiscard]] std::span<const std::byte> logical_ir_sha256() const noexcept;
    /** @return The directory that owns this mapped pack and its generated SDK children. */
    [[nodiscard]] std::wstring_view artifact_directory() const noexcept;
    /** @return Complete borrowed string-section bytes. */
    [[nodiscard]] std::span<const std::byte> string_bytes() const noexcept;
    /** @return One validated length-owned UTF-8 string. */
    [[nodiscard]] std::string_view string(format::StringRef reference) const noexcept;
    /** @return Every activity row in canonical generator order. */
    [[nodiscard]] std::span<const format::Activity> activities() const noexcept;
    /** @return Every scenario row in canonical generator order. */
    [[nodiscard]] std::span<const format::Scenario> scenarios() const noexcept;
    /** @return Every bubble row in canonical generator order. */
    [[nodiscard]] std::span<const format::Bubble> bubbles() const noexcept;
    /** @return Every state row in canonical generator order. */
    [[nodiscard]] std::span<const format::State> states() const noexcept;
    /** @return Every reusable object row in canonical generator order. */
    [[nodiscard]] std::span<const format::Object> objects() const noexcept;
    /** @return Every scenario occurrence row in canonical generator order. */
    [[nodiscard]] std::span<const format::Occurrence> occurrences() const noexcept;
    /** @return Every object slot row in canonical generator order. */
    [[nodiscard]] std::span<const format::Slot> slots() const noexcept;
    /** @return Every typed text row in canonical generator order. */
    [[nodiscard]] std::span<const format::Text> texts() const noexcept;
    /** @return Every capability row in canonical generator order. */
    [[nodiscard]] std::span<const format::Capability> capabilities() const noexcept;
    /** @return Every evidence gate row in canonical generator order. */
    [[nodiscard]] std::span<const format::Gate> gates() const noexcept;
    /** @return Every refused exposure row in canonical generator order. */
    [[nodiscard]] std::span<const format::Refusal> refusals() const noexcept;
    /** @return Every exact actor-class row in canonical generator order. */
    [[nodiscard]] std::span<const format::ActorClass> actor_classes() const noexcept;
    /** @return Every ordered actor RSAT descriptor row. */
    [[nodiscard]] std::span<const format::RsatDescriptor> rsat_descriptors() const noexcept;
    /** @return Every exact actor RSAT schema row. */
    [[nodiscard]] std::span<const format::RsatSchema> rsat_schemas() const noexcept;
    /** @return Every raw actor RSAT schema-field row. */
    [[nodiscard]] std::span<const format::RsatField> rsat_fields() const noexcept;
    /** @return Every exact scenario-local squad row. */
    [[nodiscard]] std::span<const format::Squad> squads() const noexcept;
    /** @return Every authored squad-member row. */
    [[nodiscard]] std::span<const format::SquadMember> squad_members() const noexcept;
    /** @return Every exact squad-anchor row. */
    [[nodiscard]] std::span<const format::SquadAnchor> squad_anchors() const noexcept;
    /** @return Every exact descriptor-relative type-43 package resource. */
    [[nodiscard]] std::span<const format::AuthoredSceneResource>
    authored_scene_resources() const noexcept;
    /** @return Every exact same-object type-43 to squad-slot edge. */
    [[nodiscard]] std::span<const format::AuthoredSceneSquadEdge>
    authored_scene_squad_edges() const noexcept;
    /** @return Every exact type-38 task target. */
    [[nodiscard]] std::span<const format::TaskTarget> task_targets() const noexcept;
    /** @return Every localized authored dialogue-line alias. */
    [[nodiscard]] std::span<const format::DialogueCueText> dialogue_cue_texts() const noexcept;
    /** @return Every localized bounded directive-element alias. */
    [[nodiscard]] std::span<const format::DirectiveElement> directive_elements() const noexcept;
    /** @return Every canonical activity-binding candidate or evidence tag. */
    [[nodiscard]] std::span<const format::ActivityBindingTag>
    activity_binding_tags() const noexcept;
    /** @return Every exact package locator supporting an activity binding. */
    [[nodiscard]] std::span<const format::ActivityBindingLocator>
    activity_binding_locators() const noexcept;
    /** @return Every installed compiled object-behavior root. */
    [[nodiscard]] std::span<const format::BehaviorProgram> behavior_programs() const noexcept;
    /** @return Every exact object-local channel input read by those roots. */
    [[nodiscard]] std::span<const format::BehaviorInput> behavior_inputs() const noexcept;
    /** @return Every exact object-local channel write authored by those roots. */
    [[nodiscard]] std::span<const format::BehaviorChannelWrite>
    behavior_channel_writes() const noexcept;
    /** @return Every exact activity-reached actor owner of a behavior root. */
    [[nodiscard]] std::span<const format::BehaviorOwner> behavior_owners() const noexcept;
    /** @return Every exact squad and occurrence path to a behavior owner. */
    [[nodiscard]] std::span<const format::BehaviorActivityBinding>
    behavior_activity_bindings() const noexcept;
    /** @return Every executable-derived actor command message schema. */
    [[nodiscard]] std::span<const format::ActorMessageSchema>
    actor_message_schemas() const noexcept;
    /** @return Every executable-derived actor command definition. */
    [[nodiscard]] std::span<const format::ActorCommandDefinition>
    actor_command_definitions() const noexcept;
    /** @return Every actor's package behavior config and engine faction default. */
    [[nodiscard]] std::span<const format::ActorBehaviorProfile>
    actor_behavior_profiles() const noexcept;
    /** @return Every extracted lane-0 simulation event definition. */
    [[nodiscard]] std::span<const format::SimulationEventDefinition>
    simulation_event_definitions() const noexcept;
    /** @return Every reflected schema required by extracted engine semantics. */
    [[nodiscard]] std::span<const format::RuntimeSchema> runtime_schemas() const noexcept;
    /** @return Every exact field owned by an extracted runtime schema. */
    [[nodiscard]] std::span<const format::RuntimeField> runtime_fields() const noexcept;
    /** @return The complete universal executable reflection type registry. */
    [[nodiscard]] std::span<const format::RuntimeTypeDefinition>
    runtime_type_definitions() const noexcept;
    /** @return Every installed SObject RSAT in package-tag order. */
    [[nodiscard]] std::span<const format::SobjectRsat> sobject_rsats() const noexcept;
    /** @return Every ordered component descriptor owned by installed SObject RSATs. */
    [[nodiscard]] std::span<const format::SobjectRsatDescriptor>
    sobject_rsat_descriptors() const noexcept;
    /** @return Every executable-derived channel-2 entity type contract. */
    [[nodiscard]] std::span<const format::EntityTypeDefinition>
    entity_type_definitions() const noexcept;
    /** @return Parsed executable-schema joins for every RSAT field row. */
    [[nodiscard]] std::span<const format::SobjectRsatFieldBinding>
    sobject_rsat_field_bindings() const noexcept;
    /** @return Every state-machine state name declared by an actor class, in actor-class order. */
    [[nodiscard]] std::span<const format::ActorStateName> actor_state_names() const noexcept;
    [[nodiscard]] std::span<const format::ActorSequenceTable>
    actor_sequence_tables() const noexcept;
    [[nodiscard]] std::span<const format::ActorSequenceEntry>
    actor_sequence_entries() const noexcept;
    [[nodiscard]] std::span<const format::ActorSequenceBinding>
    actor_sequence_bindings() const noexcept;

private:
    friend bool load(void* module,
                     const ExpectedIdentity& expected,
                     std::shared_ptr<Catalog>& output,
                     Status& result) noexcept;
    friend bool load_path_expected(const wchar_t* path,
                                   const ExpectedIdentity& expected,
                                   std::shared_ptr<Catalog>& output,
                                   Status& result) noexcept;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
    friend bool
    load_path(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept;
    friend bool load_path_for_test(const wchar_t* path,
                                   const std::array<std::byte, 32>& expectedPayloadSha256,
                                   std::shared_ptr<Catalog>& output,
                                   Status& result) noexcept;
#endif
    friend bool valid_catalog(const Catalog& value) noexcept;

    void* file_{};
    void* mapping_{};
    const std::byte* view_{};
    std::size_t size_{};
    const format::Header* header_{};
    core::path::Buffer artifactDirectory_{};
};

using Snapshot = std::shared_ptr<const Catalog>;

/** Selection state is captured while the caller owns its ActivityClient lock. */
struct Selection final {
    state::activity::SessionBinding binding{};
    std::size_t matchingLinks{};
    std::uint64_t activityClientGeneration{};
};

/** A bound view pins catalog, session, ActivityClient, activity, and scenario identity. */
struct BoundView final {
    Snapshot catalog{};
    state::activity::SessionBinding binding{};
    std::uint64_t activityClientGeneration{};
    std::uint32_t activityRow{format::kAbsentIndex};
    std::uint32_t scenarioRow{format::kAbsentIndex};
};

/** Publishes ready, missing, or invalid against one catalog-authorized identity. */
void initialize(void* module, const ExpectedIdentity& expected) noexcept;
/** Atomically replaces the current snapshot only when the authorized pack validates. */
[[nodiscard]] bool reload(void* module, const ExpectedIdentity& expected) noexcept;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned regression fixture in focused test binaries only. */
void initialize(void* module) noexcept;
/** Reloads the compile-pinned regression fixture in focused test binaries only. */
[[nodiscard]] bool reload(void* module) noexcept;
#endif
/** Withdraws the current immutable mapping. */
void shutdown() noexcept;
/** @return Current catalog publication state. */
[[nodiscard]] Status status() noexcept;
/** @return Stable panel text for one load or binding result. */
[[nodiscard]] const char* status_name(Status value) noexcept;
/** @return Stable diagnostic text for one initial mission-plan result. */
[[nodiscard]] const char* status_name(MissionSeedStatus value) noexcept;
/** @return Stable diagnostic text for one authored-scene seed result. */
[[nodiscard]] const char* status_name(AuthoredSceneSeedStatus value) noexcept;
/** @return Shared ownership of the current validated catalog. */
[[nodiscard]] Snapshot snapshot() noexcept;
/** Resolves one route only from the exact loaded SDK registry. */
[[nodiscard]] bool executable_communication_route(
    std::uint32_t messageId,
    middleware::bap::activity_message::wire_schema::communication::ActivityCommunicationRoute&
        output) noexcept;
/** Resolves one exact live activity and scenario without exposing an action surface. */
[[nodiscard]] Status
resolve(Snapshot catalog, const Selection& selection, BoundView& output) noexcept;
/** Checks that a bound view still names the same session and ActivityClient generation. */
[[nodiscard]] Status revalidate(const BoundView& view,
                                const state::activity::SessionBinding& currentBinding,
                                std::size_t matchingLinks,
                                std::uint64_t currentActivityClientGeneration) noexcept;

/** @return The activity row retained by a valid bound view. */
[[nodiscard]] const format::Activity* bound_activity(const BoundView& view) noexcept;
/** @return The scenario row retained by a valid bound view. */
[[nodiscard]] const format::Scenario* bound_scenario(const BoundView& view) noexcept;
/** Returns the internal and display aliases owned by one activity. */
[[nodiscard]] std::span<const format::Text>
activity_aliases(const Catalog& catalog, const format::Activity& activity) noexcept;
/** Returns capability rows owned by one activity. */
[[nodiscard]] std::span<const format::Capability>
activity_capabilities(const Catalog& catalog, const format::Activity& activity) noexcept;
/** Returns candidate activity-root tags owned by one activity. */
[[nodiscard]] std::span<const format::ActivityBindingTag>
activity_root_candidate_tags(const Catalog& catalog, const format::Activity& activity) noexcept;
/** Returns same-name scenario candidates owned by one activity. */
[[nodiscard]] std::span<const format::ActivityBindingTag>
activity_scenario_name_candidate_tags(const Catalog& catalog,
                                      const format::Activity& activity) noexcept;
/** Returns the complete canonical root-evidence union owned by one activity. */
[[nodiscard]] std::span<const format::ActivityBindingTag>
activity_evidence_root_tags(const Catalog& catalog, const format::Activity& activity) noexcept;
/** Returns exact package payload locators supporting one activity classification. */
[[nodiscard]] std::span<const format::ActivityBindingLocator>
activity_binding_locators(const Catalog& catalog, const format::Activity& activity) noexcept;
/** Returns the contiguous catalog-global Host API capability suffix. */
[[nodiscard]] std::span<const format::Capability>
host_capabilities(const Catalog& catalog) noexcept;
/** Returns bubbles owned by one scenario. */
[[nodiscard]] std::span<const format::Bubble>
scenario_bubbles(const Catalog& catalog, const format::Scenario& scenario) noexcept;
/** Returns states owned by one scenario. */
[[nodiscard]] std::span<const format::State>
scenario_states(const Catalog& catalog, const format::Scenario& scenario) noexcept;
/** Returns object occurrences owned by one scenario. */
[[nodiscard]] std::span<const format::Occurrence>
scenario_occurrences(const Catalog& catalog, const format::Scenario& scenario) noexcept;
/** Returns squad rows owned by one scenario from the sorted squad section. */
[[nodiscard]] std::span<const format::Squad>
scenario_squads(const Catalog& catalog, const format::Scenario& scenario) noexcept;
/** Returns states owned by one bubble. */
[[nodiscard]] std::span<const format::State> bubble_states(const Catalog& catalog,
                                                           const format::Bubble& bubble) noexcept;
/** Returns definition slots owned by one object. */
[[nodiscard]] std::span<const format::Slot> object_slots(const Catalog& catalog,
                                                         const format::Object& object) noexcept;
/**
 * Materializes the exact descriptor-bearing subset one generated object contributes to msg 5.
 * The validated generated pack owns the package identity and complete wire layout.
 */
[[nodiscard]] bool
materialize_roster_group(const Catalog& catalog,
                         const format::Object& object,
                         state::build_data::scenarios::RosterGroup& output) noexcept;
/**
 * Reports whether one generated roster group occurs in every enabled state of its scenario.
 * @param view Exact immutable activity/scenario binding.
 * @param objectTag Generated object definition tag.
 * @param registryKey Client roster key carried by the object definition.
 * @param scenarioWide Cleared, then receives the extracted occurrence result.
 * @return True when the object identity and complete scenario state set are unambiguous.
 */
[[nodiscard]] bool mission_seed_group_is_scenario_wide(const BoundView& view,
                                                       std::uint32_t objectTag,
                                                       std::uint32_t registryKey,
                                                       bool& scenarioWide) noexcept;
/**
 * Builds the exact selected-state roster seed for one bound scenario and package region.
 * The caller owns fixed output storage; on failure no partial group is reported by the summary.
 *
 * @param view Exact immutable activity/scenario binding.
 * @param requestedEffectiveRegion Exact authored membership region, `sliceSetIndex` plus
 * `stateOrdinal`, selected for the ActivityClient.
 * @param omissions Objects this mission leaves out of the seed, echoed into the output.
 * @param outputGroups Receives unique groups in exact occurrence order.
 * @param output Receives selected state identity and initial msg-5 work counts.
 * @return Ready only when one exact initial state and every descriptor-backed group close.
 */
[[nodiscard]] MissionSeedStatus
materialize_initial_mission_seed(const BoundView& view,
                                 std::int32_t requestedEffectiveRegion,
                                 std::span<const MissionSeedOmission> omissions,
                                 std::span<state::build_data::scenarios::RosterGroup> outputGroups,
                                 MissionSeedSummary& output) noexcept;
/**
 * Materializes every descriptor-backed type-43 slot owned by one generated object.
 * Each applicable slot must retain the exact authored schema join and exactly one exact package
 * resource row. The caller-owned output is cleared and `outputCount` remains zero on refusal.
 */
[[nodiscard]] AuthoredSceneSeedStatus
materialize_authored_scene_seeds(const Catalog& catalog,
                                 const format::Object& object,
                                 std::span<AuthoredSceneSeed> outputSeeds,
                                 std::size_t& outputCount) noexcept;
/** Returns package aliases owned by one slot. */
[[nodiscard]] std::span<const format::Text> slot_aliases(const Catalog& catalog,
                                                         const format::Slot& slot) noexcept;
/** Returns capability rows owned by one slot. */
[[nodiscard]] std::span<const format::Capability>
slot_capabilities(const Catalog& catalog, const format::Slot& slot) noexcept;
/** Returns all evidence gates owned by one capability. */
[[nodiscard]] std::span<const format::Gate>
capability_gates(const Catalog& catalog, const format::Capability& capability) noexcept;
/** Returns all refused exposures owned by one capability. */
[[nodiscard]] std::span<const format::Refusal>
capability_refusals(const Catalog& catalog, const format::Capability& capability) noexcept;
/** Returns stable refusal reasons owned by one refused exposure. */
[[nodiscard]] std::span<const format::Text>
refusal_reason_codes(const Catalog& catalog, const format::Refusal& refusal) noexcept;
/** Returns the ordered RSAT descriptors owned by one actor class. */
[[nodiscard]] std::span<const format::RsatDescriptor>
actor_class_descriptors(const Catalog& catalog, const format::ActorClass& actorClass) noexcept;
/** Returns the exact ordered field rows owned by one RSAT schema. */
[[nodiscard]] std::span<const format::RsatField>
rsat_schema_fields(const Catalog& catalog, const format::RsatSchema& schema) noexcept;
/** Returns the schema named by one descriptor from the same catalog. */
[[nodiscard]] const format::RsatSchema*
rsat_descriptor_schema(const Catalog& catalog, const format::RsatDescriptor& descriptor) noexcept;
/** Finds one actor command by its generated catalog name. */
[[nodiscard]] const format::ActorCommandDefinition*
actor_command_by_name(const Catalog& catalog, std::string_view name) noexcept;
/** Finds one actor message schema by its generated catalog name. */
[[nodiscard]] const format::ActorMessageSchema*
actor_message_schema_by_name(const Catalog& catalog, std::string_view name) noexcept;
/** Returns the behavior profile owned by one actor class from the same catalog. */
[[nodiscard]] const format::ActorBehaviorProfile*
actor_behavior_profile(const Catalog& catalog, const format::ActorClass& actorClass) noexcept;
/** Finds one simulation event by its generated catalog name. */
[[nodiscard]] const format::SimulationEventDefinition*
simulation_event_by_name(const Catalog& catalog, std::string_view name) noexcept;
/** Finds one extracted runtime schema by definition handle. */
[[nodiscard]] const format::RuntimeSchema* runtime_schema_by_handle(const Catalog& catalog,
                                                                    std::uint32_t handle) noexcept;
/** Returns the exact reflected fields owned by one runtime schema. */
[[nodiscard]] std::span<const format::RuntimeField>
runtime_schema_fields(const Catalog& catalog, const format::RuntimeSchema& schema) noexcept;
/** Finds one universal reflection type by its six-bit selector. */
[[nodiscard]] const format::RuntimeTypeDefinition*
runtime_type_by_code(const Catalog& catalog,
                     format::RuntimeCodecFamily codecFamily,
                     std::uint32_t typeCode) noexcept;
/** Finds one installed SObject RSAT by its package tag. */
[[nodiscard]] const format::SobjectRsat* sobject_rsat_by_tag(const Catalog& catalog,
                                                             std::uint32_t rsatTag) noexcept;
/** Returns the exact ordered component descriptors owned by one SObject RSAT. */
[[nodiscard]] std::span<const format::SobjectRsatDescriptor>
sobject_rsat_descriptors(const Catalog& catalog, const format::SobjectRsat& rsat) noexcept;
/** Finds one channel-2 entity type by its generated name. */
[[nodiscard]] const format::EntityTypeDefinition*
entity_type_by_name(const Catalog& catalog, std::string_view name) noexcept;
/** Finds one channel-2 entity type by its two-bit wire value. */
[[nodiscard]] const format::EntityTypeDefinition*
entity_type_by_value(const Catalog& catalog, std::uint32_t entityType) noexcept;
/** Returns the parsed executable-schema join for one RSAT field row. */
[[nodiscard]] const format::SobjectRsatFieldBinding*
sobject_rsat_field_binding(const Catalog& catalog, const format::RsatField& field) noexcept;
/** Returns authored members owned by one squad. */
[[nodiscard]] std::span<const format::SquadMember>
squad_members(const Catalog& catalog, const format::Squad& squad) noexcept;
/** Returns exact anchors owned by one squad. */
[[nodiscard]] std::span<const format::SquadAnchor>
squad_anchors(const Catalog& catalog, const format::Squad& squad) noexcept;
/** Returns all exact authored resources retained for one type-43 slot. */
[[nodiscard]] std::span<const format::AuthoredSceneResource>
slot_authored_scene_resources(const Catalog& catalog, const format::Slot& slot) noexcept;
/** Returns all exact same-object squad edges owned by one type-43 slot. */
[[nodiscard]] std::span<const format::AuthoredSceneSquadEdge>
slot_authored_scene_squad_edges(const Catalog& catalog, const format::Slot& slot) noexcept;
/** Resolves one validated scene edge to its exact type-1 target slot. */
[[nodiscard]] const format::Slot*
authored_scene_linked_squad_slot(const Catalog& catalog,
                                 const format::AuthoredSceneSquadEdge& edge) noexcept;
/** Returns the state-machine state names one actor class declares, in authored order. */
[[nodiscard]] std::span<const format::ActorStateName>
actor_class_state_names(const Catalog& catalog, std::uint32_t actorClassIndex) noexcept;
/**
 * Collects the distinct state names the squad a type-42 sensor drives declares.
 * @param slotRow Catalog row of the sensor slot.
 * @param output Cleared to the written prefix; the rest is untouched.
 * @return Names written, in first-seen order, or zero when the sensor reaches no declaring actor.
 */
[[nodiscard]] std::size_t performance_state_names(const Catalog& catalog,
                                                  const format::Scenario& scenario,
                                                  std::uint32_t slotRow,
                                                  std::span<std::uint32_t> output) noexcept;
/** Returns all exact objective targets owned by one type-38 task slot. */
[[nodiscard]] std::span<const format::TaskTarget>
slot_task_targets(const Catalog& catalog, const format::Slot& slot) noexcept;
/** Resolves one validated task target to its exact type-3 objective slot. */
[[nodiscard]] const format::Slot*
task_linked_objective_slot(const Catalog& catalog, const format::TaskTarget& target) noexcept;
} // namespace sunrise::state::activity_sdk
