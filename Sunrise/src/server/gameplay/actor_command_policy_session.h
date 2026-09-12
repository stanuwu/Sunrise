#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../middleware/gameplay/external/actor_command_runtime_codec.h"
#include "../../middleware/gameplay/external/actor_entity_registry.h"
#include "../../middleware/gameplay/external/simulation_event_runtime_codec.h"
#include "../../state/activity/definition.h"
#include "../../state/activity_sdk/format.h"
#include "../../state/activity_sdk/generated_world/runtime.h"
#include "../../state/activity_sdk/runtime.h"
#include "group/group_host_sessions.h"
#include "peer/peer_transport.h"

namespace sunrise::server::gameplay::actor_command_policy {

/** One policy row per host group session, so every advertised host can carry one. */
inline constexpr std::size_t kSessionCapacity = group::kHostSessionCapacity;
/** SDK actor-class selections one squad can hold. Class indices are one byte wide. */
inline constexpr std::size_t kSelectedActorClassCapacity =
    (std::numeric_limits<std::uint8_t>::max)();
/** Exact authored squads one mission policy can select. */
inline constexpr std::size_t kSelectedSquadCapacity = 32;
/** Live squad entities retained from channel 2. */
inline constexpr std::size_t kSquadEntityCapacity = 64;
/** Actor tokens one live squad update can name. */
inline constexpr std::size_t kSquadActorCapacity = 80;
/** Queued commands one session can hold. One lane-0 write can carry the whole ledger. */
inline constexpr std::size_t kOutputCapacity =
    middleware::gameplay::external::kSimulationEventCapacity;
/** Damage restores one session can hold at once. */
inline constexpr std::size_t kReplayCapacity = 4;
/** Transmissions one command rides before it is dropped. */
inline constexpr std::uint8_t kMaximumAttempts = 4;

/** Where one output row sits between staging and its acknowledgement. */
enum class OutputState : std::uint8_t {
    empty,
    queued,
    inFlight,
};

/** Why one output row was queued, which decides its rollback. */
enum class OutputPurpose : std::uint8_t {
    policyCommand,
    restoreCommand,
    replay,
};

/** One encoded command waiting for, or riding, a lane-0 transmission. */
struct OutputRow final {
    middleware::gameplay::external::RuntimeEventDraft draft{};
    middleware::gameplay::external::EntityToken target{};
    std::uint64_t transmissionId{};
    std::uint32_t replayIndex{state::activity_sdk::format::kAbsentIndex};
    OutputState state{OutputState::empty};
    OutputPurpose purpose{OutputPurpose::policyCommand};
    std::uint8_t attempts{};
};

/** One retained event held behind its restore barrier. */
struct ReplayRow final {
    middleware::gameplay::external::EventReplayTransaction transaction{};
    middleware::gameplay::external::EntityToken target{};
    bool occupied{};
};

/** Authored ClientRef identity of one selected mission squad. */
struct SelectedSquad final {
    std::uint32_t registryKey{};
    std::uint32_t slotIndex{};
    std::uint8_t slotType{};
    bool provoked{};
};

/**
 * A squad's catalog slot row must resolve to the authored index carried by actor references.
 * @param catalog Pinned SDK rows containing the squad.
 * @param squad Exact runnable squad whose identity is requested.
 * @param output Receives the wire reference; cleared on failure.
 * @return True when the object, slot and native squad schemas agree.
 */
[[nodiscard]] inline bool resolve_selected_squad(const state::activity_sdk::Catalog& catalog,
                                                 const state::activity_sdk::format::Squad& squad,
                                                 SelectedSquad& output) noexcept {
    namespace format = state::activity_sdk::format;
    output = {};
    const auto slots = catalog.slots();
    const auto objects = catalog.objects();
    if ((squad.flags & format::kSquadRunnableMask) != format::kSquadRunnableMask
        || squad.slotIndex >= slots.size() || squad.objectIndex >= objects.size()) {
        return false;
    }
    const auto& slot = slots[squad.slotIndex];
    const auto key = objects[squad.objectIndex].objectKey;
    if (key == 0 || key == format::kAbsentIndex || slot.objectIndex != squad.objectIndex
        || slot.slotType != format::kSquadSlotType
        || slot.componentClass != format::kSquadComponentClass
        || slot.authSchema != format::kSquadAuthSchema
        || slot.senseSchema != format::kSquadSenseSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0
        || slot.slotIndex
               > static_cast<std::uint32_t>((std::numeric_limits<std::int16_t>::max)())) {
        return false;
    }
    output.registryKey = key;
    output.slotType = static_cast<std::uint8_t>(slot.slotType);
    output.slotIndex = slot.slotIndex;
    return true;
}

/** One live type-1 entity and the actor tokens in its latest update. */
struct SquadEntityRow final {
    middleware::gameplay::external::EntityToken token{};
    std::array<middleware::gameplay::external::EntityToken, kSquadActorCapacity> actors{};
    std::uint32_t registryKey{};
    std::uint32_t slotIndex{};
    std::uint8_t slotType{};
    std::uint8_t actorCount{};
    bool occupied{};
};

/** One group's durable policy and transport state. */
struct SessionRow final {
    state::activity::SessionBinding binding{};
    state::activity_sdk::Snapshot catalog{};
    state::activity_sdk::generated_world::GeneratedWorldView worldView{};
    middleware::gameplay::external::ActorEntityRegistry actors{};
    std::array<std::uint32_t, kSelectedActorClassCapacity> actorClasses{};
    std::array<SelectedSquad, kSelectedSquadCapacity> selectedSquads{};
    std::array<SquadEntityRow, kSquadEntityCapacity> squads{};
    std::array<OutputRow, kOutputCapacity> outputs{};
    std::array<ReplayRow, kReplayCapacity> replays{};
    std::uint64_t groupSessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t activityClientGeneration{};
    peer::LinkIdentity linkIdentity{};
    std::uint32_t actorEventIndex{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t damageEventIndex{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t messageIndex{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t commandIndex{state::activity_sdk::format::kAbsentIndex};
    std::int32_t policyValue{};
    std::uint8_t actorClassCount{};
    std::uint8_t selectedSquadCount{};
    bool bindingRetained{};
    bool linkIdentityRetained{};
    bool policyActive{};
    bool occupied{};
};

/** Guards every field of every session row. */
extern SRWLOCK g_lock;
extern std::array<SessionRow, kSessionCapacity> g_sessions;
/** Monotonic service counter driving the replay barriers. Zero means no slice ran yet. */
extern std::uint64_t g_serviceFrame;

/** @return True when both tokens name the same live entity incarnation. */
[[nodiscard]] inline bool
same_token(const middleware::gameplay::external::EntityToken& left,
           const middleware::gameplay::external::EntityToken& right) noexcept {
    return left.slot == right.slot && left.incarnation == right.incarnation;
}

/** @return The occupied row for one group session, or null. The caller holds the lock. */
[[nodiscard]] SessionRow* find_session(std::uint64_t groupSessionId) noexcept;

/** @return The row for one group session, allocating it when free. The caller holds the lock. */
[[nodiscard]] SessionRow* find_or_create_session(std::uint64_t groupSessionId) noexcept;

/**
 * Known actor-source state overrides legacy network-squad membership, including a clear.
 * @param session Row holding the actor and exact selected squad references.
 * @param target Token whose current membership is checked.
 * @param selectedCount Selected prefix, including the shorter prefix before a policy extension.
 * @return The selected row, or kSelectedSquadCapacity when unknown or ambiguous.
 */
[[nodiscard]] inline std::size_t
selected_entity_squad(const SessionRow& session,
                      const middleware::gameplay::external::EntityToken& target,
                      std::size_t selectedCount) noexcept {
    if (target.slot >= session.actors.slots.size()) {
        return kSelectedSquadCapacity;
    }
    const auto& actor = session.actors.slots[target.slot];
    if (!actor.occupied || actor.incarnation != target.incarnation) {
        return kSelectedSquadCapacity;
    }
    selectedCount = (std::min)(selectedCount, session.selectedSquads.size());
    auto source = actor.authoredSource;
    if (const auto* world = session.worldView.snapshot(); world != nullptr) {
        state::gameplay::entity_identity::ActorSourceReference squadSource{};
        if (middleware::gameplay::external::resolve_combatant_squad_source(
                *world, source, squadSource)) {
            source = squadSource;
        }
    }
    std::size_t found = kSelectedSquadCapacity;
    for (std::size_t index = 0; index < selectedCount; ++index) {
        const auto& selected = session.selectedSquads[index];
        const auto matches = [&](std::uint32_t key, std::uint8_t type, std::uint32_t slot) {
            return selected.registryKey == key && selected.slotType == type
                   && selected.slotIndex == slot;
        };
        const bool member =
            source.known
                ? source.present && matches(source.key, source.type, source.index)
                : std::any_of(session.squads.begin(), session.squads.end(), [&](const auto& row) {
                      return row.occupied && row.actorCount <= row.actors.size()
                             && matches(row.registryKey, row.slotType, row.slotIndex)
                             && std::any_of(
                                 row.actors.begin(),
                                 row.actors.begin() + row.actorCount,
                                 [&](const auto& token) { return same_token(token, target); });
                  });
        if (member) {
            if (found != kSelectedSquadCapacity) {
                return kSelectedSquadCapacity;
            }
            found = index;
        }
    }
    return found;
}

/** Only one exact squad may own a policy actor. */
[[nodiscard]] inline bool
selected_entity_member(const SessionRow& session,
                       const middleware::gameplay::external::EntityToken& target,
                       std::size_t selectedCount) noexcept {
    return selected_entity_squad(session, target, selectedCount) != kSelectedSquadCapacity;
}

/**
 * The class and exact authored membership must both be selected by this policy.
 * @param session Row holding the registry and selected classes.
 * @param target Token the caller decoded.
 * @param output Absent index, then the live actor class.
 * @return True when both policy selections match the current token.
 */
[[nodiscard]] inline bool
selected_entity_class(const SessionRow& session,
                      const middleware::gameplay::external::EntityToken& target,
                      std::uint32_t& output) noexcept {
    output = state::activity_sdk::format::kAbsentIndex;
    if (!selected_entity_member(session, target, session.selectedSquadCount)) {
        return false;
    }
    output = session.actors.slots[target.slot].actorClassIndex;
    const auto classes = std::span(session.actorClasses).first(session.actorClassCount);
    return std::find(classes.begin(), classes.end(), output) != classes.end();
}

/**
 * New members of a provoked squad inherit their native faction instead of the idle policy.
 * @param session Policy and squad phase owning the actor.
 * @param target Current actor token.
 * @param actorClassIndex Validated actor class row.
 * @param profiles Profiles from the policy's pinned catalog.
 * @param value Requested faction, replaced only for a provoked squad.
 * @return False when a required native profile is unavailable.
 */
[[nodiscard]] inline bool
resolve_policy_faction(const SessionRow& session,
                       const middleware::gameplay::external::EntityToken& target,
                       std::uint32_t actorClassIndex,
                       std::span<const state::activity_sdk::format::ActorBehaviorProfile> profiles,
                       std::int32_t& value) noexcept {
    const auto squad = selected_entity_squad(session, target, session.selectedSquadCount);
    if (squad < session.selectedSquadCount && session.selectedSquads[squad].provoked) {
        if (actorClassIndex >= profiles.size()) {
            return false;
        }
        value = profiles[actorClassIndex].defaultFaction;
    }
    return true;
}

/**
 * Encodes one command and queues it when the bounded output ledger can own it.
 * @param session Row owning the ledger and the SDK indices. The caller holds the lock.
 * @param actorClassIndex SDK actor class of the target.
 * @param target Live entity token the command names.
 * @param value Faction value the command carries.
 * @param purpose Decides how a rollback finds the row again.
 * @param replayIndex Replay row this command restores, or the absent index.
 * @return True only when the row is encoded and owned.
 */
[[nodiscard]] bool
queue_command(SessionRow& session,
              std::uint32_t actorClassIndex,
              const middleware::gameplay::external::EntityToken& target,
              std::int32_t value,
              OutputPurpose purpose,
              std::uint32_t replayIndex = state::activity_sdk::format::kAbsentIndex) noexcept;

/** Removes target state while optionally keeping its newly queued replacement command. */
inline void remove_target_state(SessionRow& session,
                                const middleware::gameplay::external::EntityToken& target,
                                const OutputRow* preserve = nullptr) noexcept {
    for (OutputRow& row : session.outputs) {
        if (&row != preserve && row.state != OutputState::empty && same_token(row.target, target)) {
            row = {};
        }
    }
    for (ReplayRow& row : session.replays) {
        if (row.occupied && same_token(row.target, target)) {
            row = {};
        }
    }
}

} // namespace sunrise::server::gameplay::actor_command_policy
