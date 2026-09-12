#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "../../../state/activity_sdk/runtime.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "external_entity_codec.h"

namespace sunrise::middleware::gameplay::external {

/** Resolves only a Type 2 descriptor's exact authored squad reference. */
[[nodiscard]] bool resolve_combatant_squad_source(
    const state::build_data::scriptables::Snapshot& world,
    const state::gameplay::entity_identity::ActorSourceReference& source,
    state::gameplay::entity_identity::ActorSourceReference& output) noexcept;

/** One registry slot mirrors the 13-bit simulation entity slot space. */
struct ActorEntitySlot final {
    std::uint32_t actorClassIndex{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t rsatTag{};
    state::gameplay::entity_identity::ActorSourceReference authoredSource{};
    std::uint8_t incarnation{};
    std::uint8_t allocationSequence{};
    bool occupied{};
};

/** Immutable SDK actor-class rows pinned by one optional published snapshot. */
struct ActorEntityCatalog final {
    state::activity_sdk::Snapshot owner{};
    std::span<const state::activity_sdk::format::ActorClass> classes{};
};

/** Pins the current SDK actor-class rows for one registry operation. */
[[nodiscard]] inline bool published_actor_entity_catalog(ActorEntityCatalog& output) noexcept {
    ActorEntityCatalog candidate{};
    candidate.owner = state::activity_sdk::snapshot();
    if (candidate.owner == nullptr) {
        return false;
    }
    candidate.classes = candidate.owner->actor_classes();
    if (candidate.classes.empty()) {
        return false;
    }
    output = std::move(candidate);
    return true;
}

/** Bounded actor classification derived only from type-0 create baselines. */
struct ActorEntityRegistry final {
    state::activity_sdk::Snapshot catalog{};
    const state::activity_sdk::format::ActorClass* classData{};
    std::size_t classCount{};
    std::array<ActorEntitySlot, kMaximumEntitySlot + 1U> slots{};
};

/** What one applied record did to the registry slot it names. */
enum class ActorEntityApplyResult : std::uint8_t {
    actorCreated,
    actorRemoved,
    membershipChanged,
    nonActor,
    unchanged,
    staleToken,
    invalid,
};

/** Applies only membership carried by the decoded actor-source component. */
[[nodiscard]] ActorEntityApplyResult apply_actor_entity_record(ActorEntityRegistry& registry,
                                                               const ActorEntityCatalog& catalog,
                                                               const EntityRecord& record) noexcept;

/** Clears the registry in place without a full-table stack temporary. */
void reset_actor_entity_registry(ActorEntityRegistry& registry) noexcept;

/** Copies current entity tokens for one SDK actor class into caller-owned storage. */
[[nodiscard]] bool actor_entity_targets(const ActorEntityRegistry& registry,
                                        std::uint32_t actorClassIndex,
                                        std::span<EntityToken> output,
                                        std::size_t& count) noexcept;

} // namespace sunrise::middleware::gameplay::external
