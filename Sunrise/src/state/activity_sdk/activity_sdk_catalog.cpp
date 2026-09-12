#include <Windows.h>

#include <algorithm>
#include <atomic>

#include "internal.h"
#include "runtime.h"

namespace sunrise::state::activity_sdk {
namespace {

std::atomic<Snapshot> g_snapshot{};
std::atomic<Status> g_status{Status::notReady};

template <typename Value>
[[nodiscard]] std::span<const Value>
rows(const format::Header* header, const std::byte* view, format::SectionIndex index) noexcept {
    if (header == nullptr || view == nullptr) {
        return {};
    }
    const format::Section& section = header->sections[static_cast<std::size_t>(index)];
    return {reinterpret_cast<const Value*>(view + section.offset), section.count};
}

/** Returns one validated child range only for a parent from the same mapping. */
template <typename Parent, typename Child>
[[nodiscard]] std::span<const Child> children(std::span<const Parent> parents,
                                              const Parent& parent,
                                              std::span<const Child> values,
                                              format::Range range) noexcept {
    const std::size_t first = range.first;
    const std::size_t count = range.count;
    if (!owns(parents, parent) || first > values.size() || count > values.size() - first) {
        return {};
    }
    return values.subspan(first, count);
}

} // namespace

/** Releases the mapped view after the last immutable snapshot owner leaves. */
Catalog::~Catalog() noexcept {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
    }
    if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(file_));
    }
}

std::span<const std::byte> Catalog::sdk_build_sha256() const noexcept {
    return header_ != nullptr ? std::span<const std::byte>(header_->sdkBuildSha256)
                              : std::span<const std::byte>{};
}

std::span<const std::byte> Catalog::payload_sha256() const noexcept {
    return header_ != nullptr ? std::span<const std::byte>(header_->payloadSha256)
                              : std::span<const std::byte>{};
}

std::span<const std::byte> Catalog::content_key_sha256() const noexcept {
    return header_ != nullptr ? std::span<const std::byte>(header_->contentKeySha256)
                              : std::span<const std::byte>{};
}

std::span<const std::byte> Catalog::logical_ir_sha256() const noexcept {
    return header_ != nullptr ? std::span<const std::byte>(header_->logicalIrSha256)
                              : std::span<const std::byte>{};
}

std::wstring_view Catalog::artifact_directory() const noexcept {
    return {artifactDirectory_.chars.data(), artifactDirectory_.length};
}

std::span<const std::byte> Catalog::string_bytes() const noexcept {
    return rows<std::byte>(header_, view_, format::SectionIndex::strings);
}

/** Resolves only refs already admitted by validation and stays safe for defensive callers. */
std::string_view Catalog::string(format::StringRef reference) const noexcept {
    const auto values = string_bytes();
    if (reference.offset == format::kAbsentIndex) {
        return {};
    }
    const std::size_t first = reference.offset;
    const std::size_t length = reference.length;
    if (first > values.size() || length > values.size() - first) {
        return {};
    }
    return {reinterpret_cast<const char*>(values.data() + first), length};
}

std::span<const format::Activity> Catalog::activities() const noexcept {
    return rows<format::Activity>(header_, view_, format::SectionIndex::activities);
}

std::span<const format::Scenario> Catalog::scenarios() const noexcept {
    return rows<format::Scenario>(header_, view_, format::SectionIndex::scenarios);
}

std::span<const format::Bubble> Catalog::bubbles() const noexcept {
    return rows<format::Bubble>(header_, view_, format::SectionIndex::bubbles);
}

std::span<const format::State> Catalog::states() const noexcept {
    return rows<format::State>(header_, view_, format::SectionIndex::states);
}

std::span<const format::Object> Catalog::objects() const noexcept {
    return rows<format::Object>(header_, view_, format::SectionIndex::objects);
}

std::span<const format::Occurrence> Catalog::occurrences() const noexcept {
    return rows<format::Occurrence>(header_, view_, format::SectionIndex::occurrences);
}

std::span<const format::Slot> Catalog::slots() const noexcept {
    return rows<format::Slot>(header_, view_, format::SectionIndex::slots);
}

std::span<const format::Text> Catalog::texts() const noexcept {
    return rows<format::Text>(header_, view_, format::SectionIndex::texts);
}

std::span<const format::Capability> Catalog::capabilities() const noexcept {
    return rows<format::Capability>(header_, view_, format::SectionIndex::capabilities);
}

std::span<const format::Gate> Catalog::gates() const noexcept {
    return rows<format::Gate>(header_, view_, format::SectionIndex::gates);
}

std::span<const format::Refusal> Catalog::refusals() const noexcept {
    return rows<format::Refusal>(header_, view_, format::SectionIndex::refusals);
}

std::span<const format::ActorClass> Catalog::actor_classes() const noexcept {
    return rows<format::ActorClass>(header_, view_, format::SectionIndex::actorClasses);
}

std::span<const format::RsatDescriptor> Catalog::rsat_descriptors() const noexcept {
    return rows<format::RsatDescriptor>(header_, view_, format::SectionIndex::rsatDescriptors);
}

std::span<const format::RsatSchema> Catalog::rsat_schemas() const noexcept {
    return rows<format::RsatSchema>(header_, view_, format::SectionIndex::rsatSchemas);
}

std::span<const format::RsatField> Catalog::rsat_fields() const noexcept {
    return rows<format::RsatField>(header_, view_, format::SectionIndex::rsatFields);
}

std::span<const format::Squad> Catalog::squads() const noexcept {
    return rows<format::Squad>(header_, view_, format::SectionIndex::squads);
}

std::span<const format::SquadMember> Catalog::squad_members() const noexcept {
    return rows<format::SquadMember>(header_, view_, format::SectionIndex::squadMembers);
}

std::span<const format::SquadAnchor> Catalog::squad_anchors() const noexcept {
    return rows<format::SquadAnchor>(header_, view_, format::SectionIndex::squadAnchors);
}

std::span<const format::AuthoredSceneResource> Catalog::authored_scene_resources() const noexcept {
    return rows<format::AuthoredSceneResource>(
        header_, view_, format::SectionIndex::authoredSceneResources);
}

std::span<const format::AuthoredSceneSquadEdge>
Catalog::authored_scene_squad_edges() const noexcept {
    return rows<format::AuthoredSceneSquadEdge>(
        header_, view_, format::SectionIndex::authoredSceneSquadEdges);
}

std::span<const format::TaskTarget> Catalog::task_targets() const noexcept {
    return rows<format::TaskTarget>(header_, view_, format::SectionIndex::taskTargets);
}

std::span<const format::DialogueCueText> Catalog::dialogue_cue_texts() const noexcept {
    return rows<format::DialogueCueText>(header_, view_, format::SectionIndex::dialogueCueTexts);
}

std::span<const format::DirectiveElement> Catalog::directive_elements() const noexcept {
    return rows<format::DirectiveElement>(header_, view_, format::SectionIndex::directiveElements);
}

std::span<const format::ActivityBindingTag> Catalog::activity_binding_tags() const noexcept {
    return rows<format::ActivityBindingTag>(
        header_, view_, format::SectionIndex::activityBindingTags);
}

std::span<const format::ActivityBindingLocator>
Catalog::activity_binding_locators() const noexcept {
    return rows<format::ActivityBindingLocator>(
        header_, view_, format::SectionIndex::activityBindingLocators);
}

std::span<const format::BehaviorProgram> Catalog::behavior_programs() const noexcept {
    return rows<format::BehaviorProgram>(header_, view_, format::SectionIndex::behaviorPrograms);
}

std::span<const format::BehaviorInput> Catalog::behavior_inputs() const noexcept {
    return rows<format::BehaviorInput>(header_, view_, format::SectionIndex::behaviorInputs);
}

std::span<const format::BehaviorChannelWrite> Catalog::behavior_channel_writes() const noexcept {
    return rows<format::BehaviorChannelWrite>(
        header_, view_, format::SectionIndex::behaviorChannelWrites);
}

std::span<const format::BehaviorOwner> Catalog::behavior_owners() const noexcept {
    return rows<format::BehaviorOwner>(header_, view_, format::SectionIndex::behaviorOwners);
}

std::span<const format::BehaviorActivityBinding>
Catalog::behavior_activity_bindings() const noexcept {
    return rows<format::BehaviorActivityBinding>(
        header_, view_, format::SectionIndex::behaviorActivityBindings);
}

std::span<const format::ActorMessageSchema> Catalog::actor_message_schemas() const noexcept {
    return rows<format::ActorMessageSchema>(
        header_, view_, format::SectionIndex::actorMessageSchemas);
}

std::span<const format::ActorCommandDefinition>
Catalog::actor_command_definitions() const noexcept {
    return rows<format::ActorCommandDefinition>(
        header_, view_, format::SectionIndex::actorCommandDefinitions);
}

std::span<const format::ActorBehaviorProfile> Catalog::actor_behavior_profiles() const noexcept {
    return rows<format::ActorBehaviorProfile>(
        header_, view_, format::SectionIndex::actorBehaviorProfiles);
}

std::span<const format::SimulationEventDefinition>
Catalog::simulation_event_definitions() const noexcept {
    return rows<format::SimulationEventDefinition>(
        header_, view_, format::SectionIndex::simulationEventDefinitions);
}

std::span<const format::RuntimeSchema> Catalog::runtime_schemas() const noexcept {
    return rows<format::RuntimeSchema>(header_, view_, format::SectionIndex::runtimeSchemas);
}

std::span<const format::RuntimeField> Catalog::runtime_fields() const noexcept {
    return rows<format::RuntimeField>(header_, view_, format::SectionIndex::runtimeFields);
}

std::span<const format::RuntimeTypeDefinition> Catalog::runtime_type_definitions() const noexcept {
    return rows<format::RuntimeTypeDefinition>(
        header_, view_, format::SectionIndex::runtimeTypeDefinitions);
}

std::span<const format::SobjectRsat> Catalog::sobject_rsats() const noexcept {
    return rows<format::SobjectRsat>(header_, view_, format::SectionIndex::sobjectRsats);
}

std::span<const format::SobjectRsatDescriptor> Catalog::sobject_rsat_descriptors() const noexcept {
    return rows<format::SobjectRsatDescriptor>(
        header_, view_, format::SectionIndex::sobjectRsatDescriptors);
}

std::span<const format::EntityTypeDefinition> Catalog::entity_type_definitions() const noexcept {
    return rows<format::EntityTypeDefinition>(
        header_, view_, format::SectionIndex::entityTypeDefinitions);
}

std::span<const format::SobjectRsatFieldBinding>
Catalog::sobject_rsat_field_bindings() const noexcept {
    return rows<format::SobjectRsatFieldBinding>(
        header_, view_, format::SectionIndex::sobjectRsatFieldBindings);
}

std::span<const format::ActorSequenceTable> Catalog::actor_sequence_tables() const noexcept {
    return rows<format::ActorSequenceTable>(
        header_, view_, format::SectionIndex::actorSequenceTables);
}
std::span<const format::ActorSequenceEntry> Catalog::actor_sequence_entries() const noexcept {
    return rows<format::ActorSequenceEntry>(
        header_, view_, format::SectionIndex::actorSequenceEntries);
}
std::span<const format::ActorSequenceBinding> Catalog::actor_sequence_bindings() const noexcept {
    return rows<format::ActorSequenceBinding>(
        header_, view_, format::SectionIndex::actorSequenceBindings);
}

std::span<const format::ActorStateName> Catalog::actor_state_names() const noexcept {
    return rows<format::ActorStateName>(header_, view_, format::SectionIndex::actorStateNames);
}

/** Atomically publishes a catalog-authorized module-relative pack or its refusal status. */
void initialize(void* module, const ExpectedIdentity& expected) noexcept {
    std::shared_ptr<Catalog> pending;
    Status result = Status::catalogInvalid;
    if (load(module, expected, pending, result)) {
        g_snapshot.store(Snapshot(std::move(pending)), std::memory_order_release);
        g_status.store(Status::ready, std::memory_order_release);
        return;
    }
    g_snapshot.store({}, std::memory_order_release);
    g_status.store(result, std::memory_order_release);
}

/** Keeps the published mapping when a catalog-authorized replacement cannot be authenticated. */
bool reload(void* module, const ExpectedIdentity& expected) noexcept {
    std::shared_ptr<Catalog> pending;
    Status result = Status::catalogInvalid;
    if (!load(module, expected, pending, result)) {
        return false;
    }
    g_snapshot.store(Snapshot(std::move(pending)), std::memory_order_release);
    g_status.store(Status::ready, std::memory_order_release);
    return true;
}

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned canonical regression fixture in test binaries only. */
void initialize(void* module) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildDigest,
                                    format::kExpectedPayloadDigest,
                                    format::kExpectedContentKeyDigest,
                                    format::kExpectedLogicalIrDigest};
    initialize(module, expected);
}

/** Reloads the compile-pinned canonical regression fixture in test binaries only. */
bool reload(void* module) noexcept {
    const ExpectedIdentity expected{format::kExpectedSdkBuildDigest,
                                    format::kExpectedPayloadDigest,
                                    format::kExpectedContentKeyDigest,
                                    format::kExpectedLogicalIrDigest};
    return reload(module, expected);
}
#endif

void shutdown() noexcept {
    g_snapshot.store({}, std::memory_order_release);
    g_status.store(Status::notReady, std::memory_order_release);
}

Status status() noexcept {
    return g_status.load(std::memory_order_acquire);
}

/** Maps every public status to stable machine-readable panel text. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::notReady:
        return "not_ready";
    case Status::ready:
        return "ready";
    case Status::missing:
        return "missing";
    case Status::wrongSdkBuild:
        return "wrong_sdk_build";
    case Status::catalogInvalid:
        return "catalog_invalid";
    case Status::missingClient:
        return "missing_client";
    case Status::ambiguousClient:
        return "ambiguous_client";
    case Status::staleSession:
        return "stale_session";
    case Status::wrongActivity:
        return "wrong_activity";
    case Status::activityJoinNotExact:
        return "activity_join_not_exact";
    case Status::missingScenarioLink:
        return "missing_scenario_link";
    case Status::staleActivityClient:
        return "stale_activity_client";
    }
    return "catalog_invalid";
}

/** Maps every mission-plan result to stable machine-readable diagnostic text. */
const char* status_name(MissionSeedStatus value) noexcept {
    switch (value) {
    case MissionSeedStatus::ready:
        return "ready";
    case MissionSeedStatus::invalidView:
        return "invalid_view";
    case MissionSeedStatus::invalidSliceSet:
        return "invalid_slice_set";
    case MissionSeedStatus::missingInitialState:
        return "missing_initial_state";
    case MissionSeedStatus::ambiguousInitialState:
        return "ambiguous_initial_state";
    case MissionSeedStatus::invalidOccurrence:
        return "invalid_occurrence";
    case MissionSeedStatus::schemaJoinNotExact:
        return "schema_join_not_exact";
    case MissionSeedStatus::invalidRosterGroup:
        return "invalid_roster_group";
    case MissionSeedStatus::rosterKeyConflict:
        return "roster_key_conflict";
    case MissionSeedStatus::groupCapacityExceeded:
        return "group_capacity_exceeded";
    }
    return "invalid_view";
}

/** Maps every authored-scene seed result to stable machine-readable diagnostic text. */
const char* status_name(AuthoredSceneSeedStatus value) noexcept {
    switch (value) {
    case AuthoredSceneSeedStatus::ready:
        return "ready";
    case AuthoredSceneSeedStatus::invalidObject:
        return "invalid_object";
    case AuthoredSceneSeedStatus::schemaMismatch:
        return "schema_mismatch";
    case AuthoredSceneSeedStatus::missingResource:
        return "missing_resource";
    case AuthoredSceneSeedStatus::ambiguousResource:
        return "ambiguous_resource";
    case AuthoredSceneSeedStatus::capacityExceeded:
        return "capacity_exceeded";
    }
    return "invalid_object";
}

Snapshot snapshot() noexcept {
    return g_snapshot.load(std::memory_order_acquire);
}

/** Fills the compiled communication route for one message id built into the executable. */
bool executable_communication_route(
    std::uint32_t messageId,
    middleware::bap::activity_message::wire_schema::communication::ActivityCommunicationRoute&
        output) noexcept {
    namespace communication = middleware::bap::activity_message::wire_schema::communication;
    output = {};
    const communication::ActivityCommunicationRoute* const route =
        communication::find_route(messageId);
    if (route == nullptr) {
        return false;
    }
    output = *route;
    return true;
}

const format::Activity* bound_activity(const BoundView& view) noexcept {
    if (view.catalog == nullptr || view.activityRow >= view.catalog->activities().size()) {
        return nullptr;
    }
    return &view.catalog->activities()[view.activityRow];
}

const format::Scenario* bound_scenario(const BoundView& view) noexcept {
    if (view.catalog == nullptr || view.scenarioRow >= view.catalog->scenarios().size()) {
        return nullptr;
    }
    return &view.catalog->scenarios()[view.scenarioRow];
}

std::span<const format::Text> activity_aliases(const Catalog& catalog,
                                               const format::Activity& activity) noexcept {
    return children(catalog.activities(), activity, catalog.texts(), activity.aliases);
}

std::span<const format::Capability>
activity_capabilities(const Catalog& catalog, const format::Activity& activity) noexcept {
    return children(catalog.activities(), activity, catalog.capabilities(), activity.capabilities);
}

std::span<const format::ActivityBindingTag>
activity_root_candidate_tags(const Catalog& catalog, const format::Activity& activity) noexcept {
    return children(catalog.activities(),
                    activity,
                    catalog.activity_binding_tags(),
                    activity.activityRootCandidateTags);
}

std::span<const format::ActivityBindingTag>
activity_scenario_name_candidate_tags(const Catalog& catalog,
                                      const format::Activity& activity) noexcept {
    return children(catalog.activities(),
                    activity,
                    catalog.activity_binding_tags(),
                    activity.scenarioNameCandidateTags);
}

std::span<const format::ActivityBindingTag>
activity_evidence_root_tags(const Catalog& catalog, const format::Activity& activity) noexcept {
    return children(
        catalog.activities(), activity, catalog.activity_binding_tags(), activity.evidenceRootTags);
}

std::span<const format::ActivityBindingLocator>
activity_binding_locators(const Catalog& catalog, const format::Activity& activity) noexcept {
    return children(catalog.activities(),
                    activity,
                    catalog.activity_binding_locators(),
                    activity.bindingLocators);
}

/** Finds the canonical contiguous host-capability suffix without allocating a filtered copy. */
std::span<const format::Capability> host_capabilities(const Catalog& catalog) noexcept {
    const auto values = catalog.capabilities();
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index].subjectKind == static_cast<std::uint32_t>(format::SubjectKind::hostApi)) {
            return values.subspan(index);
        }
    }
    return {};
}

std::span<const format::Bubble> scenario_bubbles(const Catalog& catalog,
                                                 const format::Scenario& scenario) noexcept {
    return children(catalog.scenarios(), scenario, catalog.bubbles(), scenario.bubbles);
}

std::span<const format::State> scenario_states(const Catalog& catalog,
                                               const format::Scenario& scenario) noexcept {
    return children(catalog.scenarios(), scenario, catalog.states(), scenario.states);
}

std::span<const format::Occurrence>
scenario_occurrences(const Catalog& catalog, const format::Scenario& scenario) noexcept {
    return children(catalog.scenarios(), scenario, catalog.occurrences(), scenario.occurrences);
}

/** Relies on scenario-index ordering to return one contiguous zero-copy squad range. */
std::span<const format::Squad> scenario_squads(const Catalog& catalog,
                                               const format::Scenario& scenario) noexcept {
    const auto scenarios = catalog.scenarios();
    if (!owns(scenarios, scenario)) {
        return {};
    }
    const auto values = catalog.squads();
    const std::uint32_t scenarioIndex = static_cast<std::uint32_t>(&scenario - scenarios.data());
    const auto first = std::lower_bound(
        values.begin(), values.end(), scenarioIndex, [](const format::Squad& row, auto index) {
            return row.scenarioIndex < index;
        });
    const auto last = std::upper_bound(
        first, values.end(), scenarioIndex, [](auto index, const format::Squad& row) {
            return index < row.scenarioIndex;
        });
    const std::size_t offset = static_cast<std::size_t>(first - values.begin());
    return values.subspan(offset, static_cast<std::size_t>(last - first));
}

std::span<const format::State> bubble_states(const Catalog& catalog,
                                             const format::Bubble& bubble) noexcept {
    return children(catalog.bubbles(), bubble, catalog.states(), bubble.states);
}

std::span<const format::Slot> object_slots(const Catalog& catalog,
                                           const format::Object& object) noexcept {
    return children(catalog.objects(), object, catalog.slots(), object.slots);
}

std::span<const format::Text> slot_aliases(const Catalog& catalog,
                                           const format::Slot& slot) noexcept {
    return children(catalog.slots(), slot, catalog.texts(), slot.aliases);
}

std::span<const format::Capability> slot_capabilities(const Catalog& catalog,
                                                      const format::Slot& slot) noexcept {
    return children(catalog.slots(), slot, catalog.capabilities(), slot.capabilities);
}

std::span<const format::Gate> capability_gates(const Catalog& catalog,
                                               const format::Capability& capability) noexcept {
    return children(catalog.capabilities(), capability, catalog.gates(), capability.gates);
}

std::span<const format::Refusal>
capability_refusals(const Catalog& catalog, const format::Capability& capability) noexcept {
    return children(catalog.capabilities(), capability, catalog.refusals(), capability.refusals);
}

std::span<const format::Text> refusal_reason_codes(const Catalog& catalog,
                                                   const format::Refusal& refusal) noexcept {
    return children(catalog.refusals(), refusal, catalog.texts(), refusal.reasonCodes);
}

std::span<const format::RsatDescriptor>
actor_class_descriptors(const Catalog& catalog, const format::ActorClass& actorClass) noexcept {
    return children(
        catalog.actor_classes(), actorClass, catalog.rsat_descriptors(), actorClass.descriptors);
}

std::span<const format::RsatField> rsat_schema_fields(const Catalog& catalog,
                                                      const format::RsatSchema& schema) noexcept {
    return children(catalog.rsat_schemas(), schema, catalog.rsat_fields(), schema.fields);
}

const format::RsatSchema*
rsat_descriptor_schema(const Catalog& catalog, const format::RsatDescriptor& descriptor) noexcept {
    if (!owns(catalog.rsat_descriptors(), descriptor)
        || descriptor.schemaIndex >= catalog.rsat_schemas().size()) {
        return nullptr;
    }
    return &catalog.rsat_schemas()[descriptor.schemaIndex];
}

/** @return The actor command with this name, or null when no row carries it. */
const format::ActorCommandDefinition* actor_command_by_name(const Catalog& catalog,
                                                            std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const format::ActorCommandDefinition& row : catalog.actor_command_definitions()) {
        if (catalog.string(row.name) == name) {
            return &row;
        }
    }
    return nullptr;
}

/** @return The actor message schema with this name, or null when no row carries it. */
const format::ActorMessageSchema* actor_message_schema_by_name(const Catalog& catalog,
                                                               std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const format::ActorMessageSchema& row : catalog.actor_message_schemas()) {
        if (catalog.string(row.name) == name) {
            return &row;
        }
    }
    return nullptr;
}

/** @return The profile row of this actor class, or null when the two rows disagree. */
const format::ActorBehaviorProfile*
actor_behavior_profile(const Catalog& catalog, const format::ActorClass& actorClass) noexcept {
    const auto actors = catalog.actor_classes();
    if (!owns(actors, actorClass)) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(&actorClass - actors.data());
    const auto profiles = catalog.actor_behavior_profiles();
    if (index >= profiles.size() || profiles[index].actorClassIndex != index) {
        return nullptr;
    }
    return &profiles[index];
}

/** @return The simulation event with this name, or null when no row carries it. */
const format::SimulationEventDefinition* simulation_event_by_name(const Catalog& catalog,
                                                                  std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const format::SimulationEventDefinition& row : catalog.simulation_event_definitions()) {
        if (catalog.string(row.name) == name) {
            return &row;
        }
    }
    return nullptr;
}

const format::RuntimeSchema* runtime_schema_by_handle(const Catalog& catalog,
                                                      std::uint32_t handle) noexcept {
    for (const format::RuntimeSchema& row : catalog.runtime_schemas()) {
        if (row.handle == handle) {
            return &row;
        }
    }
    return nullptr;
}

std::span<const format::RuntimeField>
runtime_schema_fields(const Catalog& catalog, const format::RuntimeSchema& schema) noexcept {
    return children(catalog.runtime_schemas(), schema, catalog.runtime_fields(), schema.fields);
}

/** @return The type this codec family gives that code, or null when no row declares it. */
const format::RuntimeTypeDefinition* runtime_type_by_code(const Catalog& catalog,
                                                          format::RuntimeCodecFamily codecFamily,
                                                          std::uint32_t typeCode) noexcept {
    for (const format::RuntimeTypeDefinition& row : catalog.runtime_type_definitions()) {
        if ((row.codecFamilies & static_cast<std::uint32_t>(codecFamily)) != 0
            && row.typeCode == typeCode) {
            return &row;
        }
    }
    return nullptr;
}

const format::SobjectRsat* sobject_rsat_by_tag(const Catalog& catalog,
                                               std::uint32_t rsatTag) noexcept {
    const auto rows = catalog.sobject_rsats();
    const auto found =
        std::lower_bound(rows.begin(), rows.end(), rsatTag, [](const auto& row, auto tag) {
            return row.rsatTag < tag;
        });
    return found != rows.end() && found->rsatTag == rsatTag ? &*found : nullptr;
}

std::span<const format::SobjectRsatDescriptor>
sobject_rsat_descriptors(const Catalog& catalog, const format::SobjectRsat& rsat) noexcept {
    return children(
        catalog.sobject_rsats(), rsat, catalog.sobject_rsat_descriptors(), rsat.descriptors);
}

/** @return The entity type with this generated name, or null when no row carries it. */
const format::EntityTypeDefinition* entity_type_by_name(const Catalog& catalog,
                                                        std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const format::EntityTypeDefinition& row : catalog.entity_type_definitions()) {
        if (catalog.string(row.name) == name) {
            return &row;
        }
    }
    return nullptr;
}

const format::EntityTypeDefinition* entity_type_by_value(const Catalog& catalog,
                                                         std::uint32_t entityType) noexcept {
    for (const format::EntityTypeDefinition& row : catalog.entity_type_definitions()) {
        if (row.entityType == entityType) {
            return &row;
        }
    }
    return nullptr;
}

/** @return The binding row of this RSAT field, or null when the two rows disagree. */
const format::SobjectRsatFieldBinding*
sobject_rsat_field_binding(const Catalog& catalog, const format::RsatField& field) noexcept {
    const auto fields = catalog.rsat_fields();
    if (!owns(fields, field)) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(&field - fields.data());
    const auto bindings = catalog.sobject_rsat_field_bindings();
    return index < bindings.size() && bindings[index].rsatFieldIndex == index ? &bindings[index]
                                                                              : nullptr;
}

std::span<const format::SquadMember> squad_members(const Catalog& catalog,
                                                   const format::Squad& squad) noexcept {
    return children(catalog.squads(), squad, catalog.squad_members(), squad.members);
}

std::span<const format::SquadAnchor> squad_anchors(const Catalog& catalog,
                                                   const format::Squad& squad) noexcept {
    return children(catalog.squads(), squad, catalog.squad_anchors(), squad.anchors);
}

/** Relies on slot-index ordering to return one contiguous zero-copy resource range. */
std::span<const format::AuthoredSceneResource>
slot_authored_scene_resources(const Catalog& catalog, const format::Slot& slot) noexcept {
    const auto slots = catalog.slots();
    if (!owns(slots, slot)) {
        return {};
    }
    const auto values = catalog.authored_scene_resources();
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(&slot - slots.data());
    const auto first =
        std::lower_bound(values.begin(), values.end(), slotIndex, [](const auto& row, auto index) {
            return row.slotIndex < index;
        });
    const auto last =
        std::upper_bound(first, values.end(), slotIndex, [](auto index, const auto& row) {
            return index < row.slotIndex;
        });
    return values.subspan(static_cast<std::size_t>(first - values.begin()),
                          static_cast<std::size_t>(last - first));
}

/** Relies on scene-slot ordering to return one contiguous zero-copy edge range. */
std::span<const format::AuthoredSceneSquadEdge>
slot_authored_scene_squad_edges(const Catalog& catalog, const format::Slot& slot) noexcept {
    const auto slots = catalog.slots();
    if (!owns(slots, slot)) {
        return {};
    }
    const auto values = catalog.authored_scene_squad_edges();
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(&slot - slots.data());
    const auto first =
        std::lower_bound(values.begin(), values.end(), slotIndex, [](const auto& row, auto index) {
            return row.sceneSlotIndex < index;
        });
    const auto last =
        std::upper_bound(first, values.end(), slotIndex, [](auto index, const auto& row) {
            return index < row.sceneSlotIndex;
        });
    return values.subspan(static_cast<std::size_t>(first - values.begin()),
                          static_cast<std::size_t>(last - first));
}

const format::Slot*
authored_scene_linked_squad_slot(const Catalog& catalog,
                                 const format::AuthoredSceneSquadEdge& edge) noexcept {
    if (!owns(catalog.authored_scene_squad_edges(), edge)
        || edge.squadSlotIndex >= catalog.slots().size()) {
        return nullptr;
    }
    return &catalog.slots()[edge.squadSlotIndex];
}

/** Relies on actor-class ordering to return one contiguous zero-copy state-name range. */
std::span<const format::ActorStateName>
actor_class_state_names(const Catalog& catalog, std::uint32_t actorClassIndex) noexcept {
    const auto values = catalog.actor_state_names();
    const auto first = std::lower_bound(
        values.begin(), values.end(), actorClassIndex, [](const auto& row, auto index) {
            return row.actorClassIndex < index;
        });
    const auto last =
        std::upper_bound(first, values.end(), actorClassIndex, [](auto index, const auto& row) {
            return index < row.actorClassIndex;
        });
    return values.subspan(static_cast<std::size_t>(first - values.begin()),
                          static_cast<std::size_t>(last - first));
}

/** Copies one performance slot's state name hashes. @return How many were written. */
std::size_t performance_state_names(const Catalog& catalog,
                                    const format::Scenario& scenario,
                                    std::uint32_t slotRow,
                                    std::span<std::uint32_t> output) noexcept {
    const auto slots = catalog.slots();
    if (slotRow >= slots.size() || output.empty()) {
        return 0;
    }
    const format::Slot& slot = slots[slotRow];
    if (slot.slotType != format::kPerformanceSlotType
        || slot.componentClass != format::kPerformanceComponentClass
        || slot.authSchema != format::kPerformanceAuthSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return 0;
    }
    // A sensor drives exactly one squad. Two edges would leave the target ambiguous.
    std::uint32_t squadSlot = format::kAbsentIndex;
    std::size_t edges = 0;
    for (const format::AuthoredSceneSquadEdge& edge :
         slot_authored_scene_squad_edges(catalog, slot)) {
        if ((edge.flags & format::kAuthoredSceneSquadPerformanceTargetExact) != 0) {
            squadSlot = edge.squadSlotIndex;
            ++edges;
        }
    }
    if (edges != 1) {
        return 0;
    }
    std::size_t written = 0;
    for (const format::Squad& squad : scenario_squads(catalog, scenario)) {
        if (squad.slotIndex != squadSlot) {
            continue;
        }
        for (const format::SquadMember& member : squad_members(catalog, squad)) {
            for (const format::ActorStateName& name :
                 actor_class_state_names(catalog, member.actorClassIndex)) {
                if (std::find(output.begin(),
                              output.begin() + static_cast<std::ptrdiff_t>(written),
                              name.nameHash)
                    != output.begin() + static_cast<std::ptrdiff_t>(written)) {
                    continue;
                }
                if (written == output.size()) {
                    return written;
                }
                output[written++] = name.nameHash;
            }
        }
    }
    return written;
}

/** Relies on task-slot ordering to return one contiguous zero-copy target range. */
std::span<const format::TaskTarget> slot_task_targets(const Catalog& catalog,
                                                      const format::Slot& slot) noexcept {
    const auto slots = catalog.slots();
    if (!owns(slots, slot)) {
        return {};
    }
    const auto values = catalog.task_targets();
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(&slot - slots.data());
    const auto first =
        std::lower_bound(values.begin(), values.end(), slotIndex, [](const auto& row, auto index) {
            return row.taskSlotIndex < index;
        });
    const auto last =
        std::upper_bound(first, values.end(), slotIndex, [](auto index, const auto& row) {
            return index < row.taskSlotIndex;
        });
    return values.subspan(static_cast<std::size_t>(first - values.begin()),
                          static_cast<std::size_t>(last - first));
}

/** @return The objective slot one task target names, or null when it names none. */
const format::Slot* task_linked_objective_slot(const Catalog& catalog,
                                               const format::TaskTarget& target) noexcept {
    const auto targets = catalog.task_targets();
    const auto slots = catalog.slots();
    if (!owns(targets, target) || target.objectiveSlotIndex >= slots.size()) {
        return nullptr;
    }
    const format::Slot& slot = slots[target.objectiveSlotIndex];
    return slot.slotType == format::kObjectiveSlotType
                   && slot.componentClass == format::kObjectiveComponentClass
                   && slot.senseSchema == format::kObjectiveSenseSchema
                   && slot.authSchema == format::kObjectiveAuthSchema
               ? &slot
               : nullptr;
}

} // namespace sunrise::state::activity_sdk
