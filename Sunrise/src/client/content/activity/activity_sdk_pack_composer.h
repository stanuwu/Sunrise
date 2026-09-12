#pragma once

#include <cstddef>
#include <vector>

#include "../../../state/activity_sdk/generation/pack_writer.h"

namespace sunrise::client::content::activity::sdk_generation::activity_enrichment_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::activity_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::behavior_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::policy_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {
struct Facts;
struct Snapshot;
} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory

namespace sunrise::client::content::activity::sdk_generation::topology_enrichment {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::topology_inventory {
struct Snapshot;
}

namespace sunrise::client::content::activity::sdk_generation::pack_composer {

namespace format = state::activity_sdk::format;
namespace pack = state::activity_sdk::generation::pack;

/** Accepted native inventories needed to close every runtime-pack section. */
struct Inputs final {
    const activity_inventory::Snapshot* activityInventory{};
    const activity_enrichment_inventory::Snapshot* activityEnrichment{};
    const topology_inventory::Snapshot* topology{};
    const topology_enrichment::Snapshot* topologyEnrichment{};
    const policy_inventory::Snapshot* policy{};
    const actor_rsat_inventory::Snapshot* actorRsat{};
    const squad_inventory::Facts* squadFacts{};
    const squad_inventory::Snapshot* squads{};
    const authored_scene_inventory::Snapshot* authoredScenes{};
    const behavior_inventory::Snapshot* behaviors{};
};

/** Owned final rows whose borrowed view remains valid until this object changes. */
struct Storage final {
    std::vector<std::byte> strings{};
    std::vector<format::Activity> activities{};
    std::vector<format::Scenario> scenarios{};
    std::vector<format::Bubble> bubbles{};
    std::vector<format::State> states{};
    std::vector<format::Object> objects{};
    std::vector<format::Occurrence> occurrences{};
    std::vector<format::Slot> slots{};
    std::vector<format::Text> texts{};
    std::vector<format::Capability> capabilities{};
    std::vector<format::Gate> gates{};
    std::vector<format::Refusal> refusals{};
    std::vector<format::ActorClass> actorClasses{};
    std::vector<format::RsatDescriptor> rsatDescriptors{};
    std::vector<format::RsatSchema> rsatSchemas{};
    std::vector<format::RsatField> rsatFields{};
    std::vector<format::Squad> squads{};
    std::vector<format::SquadMember> squadMembers{};
    std::vector<format::SquadAnchor> squadAnchors{};
    std::vector<format::AuthoredSceneResource> authoredSceneResources{};
    std::vector<format::AuthoredSceneSquadEdge> authoredSceneSquadEdges{};
    std::vector<format::TaskTarget> taskTargets{};
    std::vector<format::DialogueCueText> dialogueCueTexts{};
    std::vector<format::DirectiveElement> directiveElements{};
    std::vector<format::ActivityBindingTag> activityBindingTags{};
    std::vector<format::ActivityBindingLocator> activityBindingLocators{};
    std::vector<format::BehaviorProgram> behaviorPrograms{};
    std::vector<format::BehaviorInput> behaviorInputs{};
    std::vector<format::BehaviorChannelWrite> behaviorChannelWrites{};
    std::vector<format::BehaviorOwner> behaviorOwners{};
    std::vector<format::BehaviorActivityBinding> behaviorActivityBindings{};
    std::vector<format::ActorMessageSchema> actorMessageSchemas{};
    std::vector<format::ActorCommandDefinition> actorCommandDefinitions{};
    std::vector<format::ActorBehaviorProfile> actorBehaviorProfiles{};
    std::vector<format::SimulationEventDefinition> simulationEventDefinitions{};
    std::vector<format::RuntimeSchema> runtimeSchemas{};
    std::vector<format::RuntimeField> runtimeFields{};
    std::vector<format::RuntimeTypeDefinition> runtimeTypeDefinitions{};
    std::vector<format::SobjectRsat> sobjectRsats{};
    std::vector<format::SobjectRsatDescriptor> sobjectRsatDescriptors{};
    std::vector<format::EntityTypeDefinition> entityTypeDefinitions{};
    std::vector<format::SobjectRsatFieldBinding> sobjectRsatFieldBindings{};
    std::vector<format::ActorStateName> actorStateNames{};
    std::vector<format::ActorSequenceTable> actorSequenceTables{};
    std::vector<format::ActorSequenceEntry> actorSequenceEntries{};
    std::vector<format::ActorSequenceBinding> actorSequenceBindings{};

    /** @return Borrowed sections in exact current format order. */
    [[nodiscard]] pack::Tables tables() const noexcept;
};

/** Links accepted native inventories without building or publishing a file. */
[[nodiscard]] bool compose(const Inputs& inputs, Storage& output) noexcept;

/** Links generation-owned inventories without rerunning their validation passes. */
[[nodiscard]] bool compose_generated(const Inputs& inputs, Storage& output) noexcept;

/** Links all rows and calls the in-memory runtime-pack writer transactionally. */
[[nodiscard]] pack::Status build(const pack::Identity& identity,
                                 const Inputs& inputs,
                                 Storage& tables,
                                 std::vector<std::byte>& image,
                                 pack::Digest& payloadSha256) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::pack_composer
