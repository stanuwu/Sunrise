#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../state/activity/entity_slots/member_leases.h"
#include "../../../state/activity/mission/runtime.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/generated_world/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../host_runtime.h"
#include "mission_member_life.h"
#include "mission_output_route.h"
#include "mission_script_actor_path_sense.h"
#include "mission_script_combatant_damage_sense.h"
#include "mission_script_device_sense.h"
#include "mission_script_ghost_sense.h"
#include "mission_script_object_sense.h"
#include "mission_script_player_sense.h"
#include "mission_script_runtime.h"
#include "mission_script_squad_sense.h"
#include "mission_script_vm.h"
#include "mission_trigger_observation.h"

// What the mission-runtime translation units share: the instance table and service slice,
// the attach pipeline, the two Host feeds and the VM callback, the panel rows, the delivery state
// machine, the intent fan-out, and the Sense and host-state edges.

namespace sunrise::server::bap {
struct ActivityMissionSeedPlan;
} // namespace sunrise::server::bap

namespace sunrise::server::activity::mission {

namespace sdk = state::activity_sdk;
namespace generated = state::activity_sdk::generated_world;
namespace format = state::activity_sdk::format;
namespace mission_state = state::activity::mission;

/** A restored scene intent must still name its complete captured parent set. */
[[nodiscard]] bool scene_spawn_sources_match(const sdk::BoundView& view,
                                             const lua_vm::Intent& intent) noexcept;

enum class ProgramStatus : std::uint8_t {
    none,
    loaded,
    missing,
    fileError,
    sourceTooLarge,
    programError,
};

enum class DeliveryStage : std::uint8_t {
    idle,
    awaitingHostCommit,
    awaitingTransport,
    awaitingCancel,
};

/** Stable result classes bound repeated attach logs. */
enum class AttachResult : std::uint8_t {
    none,
    catalogUnavailable,
    noActivityLink,
    sdkStatus,
    generatedWorldStatus,
    capacity,
    noScript,
    scriptFileError,
    sourceTooLarge,
    programError,
    ready,
};

/** Copies text into a fixed buffer, truncating it, and always leaves it terminated. */
template <std::size_t Capacity>
void copy_text(std::array<char, Capacity>& output, std::string_view value) noexcept {
    output = {};
    const std::size_t length = (std::min)(value.size(), output.size() - 1);
    std::copy_n(value.data(), length, output.data());
}

/** Watched trigger volumes retained per instance. */
constexpr std::size_t kTriggerOccupancyCapacity = 32;
/** Watched squads retained per instance. A social destination places well over a hundred. */
constexpr std::size_t kSquadObservationCapacity = 160;
/** Watched authored scenes retained per instance. */
constexpr std::size_t kSceneObservationCapacity = 32;
/** Watched objective sensors retained per instance. */
constexpr std::size_t kObjectiveObservationCapacity = 32;
static_assert(kSquadObjectiveGroupCount == host::kSquadObjectiveGroupCount);
/** Watched Ghost links, damage monitors, interactable objects and named actors per instance. */
constexpr std::size_t kGhostObservationCapacity = kGhostLinkCapacity;
constexpr std::size_t kDamageObservationCapacity = 8;
constexpr std::size_t kObjectInteractionObservationCapacity = 64;
constexpr std::size_t kActorPathObservationCapacity = 64;
/** Watched device levels retained per instance. */
constexpr std::size_t kDeviceObservationCapacity = 128;
/** One objective sensor carries this many objective blocks. */
constexpr std::size_t kObjectiveCapacity = 24;
/** One objective block carries this many task counters. */
constexpr std::size_t kObjectiveTaskCapacity = 24;
/** How long a committed Host output may take to reach transport before the delivery faults. */
constexpr std::uint64_t kTransportTimeoutMs = 15'000;
/** How long an enqueued Host output may take to reach the reducer before it is reconciled. */
constexpr std::uint64_t kHostCommitTimeoutMs = 2'000;
/** The route, which may not be joined, then every joined private link of the party. */
constexpr std::size_t kLiveLinkCapacity = state::activity::entity_slots::kMemberLeaseRowCount + 1;

/** Last occupancy seen for one watched volume, so only a change raises an edge. */
struct TriggerOccupancy final {
    /** Counter continuity of the volume's reports, for triggerState events only. */
    trigger_observation::Tracker tracker{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool occupied{};
    bool used{};
};

/** Last movement and delivery level seen for one named actor, so only a change raises an event. */
struct ActorPathObservation final {
    ActorPathLevel level{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last health, shield and revision seen for one damage monitor. */
struct DamageObservation final {
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    std::int32_t revision{};
    float health{-1.0F};
    float shield{-1.0F};
    bool used{};
};

/** Last damage-pool observation for one exact Type-2 source. */
struct CombatantDamageObservation final {
    CombatantDamageLevel level{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last published pools of one entity an admitted entity source reports. */
struct EntityDamageObservation final {
    state::gameplay::entity_identity::Token token{};
    state::gameplay::entity_identity::ActorSourceReference actorSource{};
    state::gameplay::entity_identity::DamagePools damage{};
    std::uint64_t allocationDomain{};
    /** Link whose entity source reported it. Network slots are unique only within one source. */
    std::uint64_t sourceGeneration{};
    std::uint8_t allocationSequence{}, allocationEpoch{};
    /** Set when a queued publication was refused, so the next poll publishes these pools. */
    bool withheld{};
    /** Set when the last poll of the source found the entity still listed. */
    bool listed{};
    bool used{};
};
/** Last reported current values for one exact Type-23 source. */
struct DeviceObservation final {
    DeviceLevel level{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last object and interaction level seen for one interactable object. */
struct ObjectInteractionObservation final {
    ObjectInteractionLevel level{};
    /** Link whose reports the shared object state was read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last Ghost-link level seen for one sensor. */
struct GhostObservation final {
    GhostLevel level{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** The squad levels one row's edges were last derived from. */
struct SquadPublishedLevels final {
    SquadObjectiveCosts objectiveCosts{};
    std::array<std::int32_t, host::kSquadSlotCapacity> slotCounts{};
    std::int32_t aliveCount{};
    std::uint8_t slotCountLength{};
    bool removalFlag{};
    bool operator==(const SquadPublishedLevels&) const = default;
};

/** Last squad counters seen for one watched object, so only a change raises an event. */
struct SquadObservation final {
    SquadObjectiveCosts objectiveCosts{};
    std::array<std::int32_t, host::kSquadSlotCapacity> slotCounts{};
    /**
     * A link's deltas are folded into the levels above while another link simulates the squad, so
     * its next admitted report compares with what the script last saw, not with the fold.
     */
    SquadPublishedLevels published{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::int32_t aliveCount{};
    /** Last spawn generation the squad Sense echoed from its placement Auth. */
    std::int32_t spawnGeneration{};
    std::uint16_t slotIndex{};
    std::uint8_t slotCountLength{};
    bool removalFlag{};
    bool hasSpawnGeneration{};
    /** An alive count was reported; until then aliveCount is a zero placeholder. */
    bool populationKnown{};
    bool used{};
};
/** Last completion latch seen for one watched authored scene, so only the edge raises an event. */
struct SceneObservation final {
    /** Output keys this run has reported. A scene's Sense list holds at most 32 keys. */
    std::array<std::uint32_t, 32> outputs{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    /** Activation token of the run the outputs belong to; a new token starts a new run. */
    std::int32_t token{};
    std::uint16_t slotIndex{};
    std::uint8_t outputCount{};
    bool completed{};
    bool used{};
};

/** Last task counters seen for one watched objective sensor, so only a rise raises an event. */
struct ObjectiveObservation final {
    std::array<std::array<std::uint8_t, kObjectiveTaskCapacity>, kObjectiveCapacity> counters{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

/** Last peer session seen sharing this destination, so only a change raises an event. */
struct SessionRosterWatch final {
    std::uint64_t sessionId{};
    std::uint64_t createdRevision{};
    std::uint64_t memberKey{};
    std::uint64_t joinIdentity{};
    std::uint64_t joinedRevision{};
    bool used{};
};

/** Per-member levels never merge a guest's local sensor deltas with another client's. */
struct MemberInputState final {
    MissionMemberLife life{};
    std::array<TriggerOccupancy, kTriggerOccupancyCapacity> triggerOccupancy{};
    std::array<GhostObservation, kGhostObservationCapacity> ghostObservations{};
    std::array<ObjectInteractionObservation, kObjectInteractionObservationCapacity>
        objectInteractionObservations{};
    bool present{};
};

/** The entity source last polled for one link. */
struct EntityDamageSource final {
    state::gameplay::entity_identity::Source source{};
    /** Set once this source's rows ran out, so its warning is logged once. */
    bool atCapacity{};
};

/** Everything one bound mission program owns: its VM, views, delivery state and counters. */
struct RuntimeInstance final {
    mission_state::AttemptState attempt{};
    std::uint64_t dispatchAttemptGeneration{};
    std::uint64_t dispatchInputSequence{};
    lua_vm::Vm vm{};
    sdk::BoundView view{};
    generated::GeneratedWorldView worldView{};
    /**
     * Pins of the binding's retained Auth bodies, copied once per Sense dispatch that reads shared
     * simulation and once per entity poll. They say which link simulates each target, so every
     * admission in that pass reads one copy.
     */
    std::array<host::ScriptablePin, host::kScriptablePinCapacity> pins{};
    std::size_t pinCount{};
    /**
     * The bubbles a retained body of each ClientRef belongs to, read from the catalog once when
     * the view binds its catalog and scenario, so following the grant holders costs each pin one
     * lookup instead of a walk over the catalog.
     */
    std::vector<PinBubbleRow> pinBubbles{};
    /** The full retained bodies, copied only for a squad edge that reads its objective. */
    std::vector<host::PendingScriptableOverride> estate{};
    /** Set once `estate` holds the copy of the pass the pins belong to. */
    bool estateCopied{};
    /** The route and every joined private link of the binding, read once per service pass. */
    std::array<std::uint64_t, kLiveLinkCapacity> liveLinks{};
    std::size_t liveLinkCount{};
    /** The entity source last polled for each link, at most one per ActivityClient generation. */
    std::array<EntityDamageSource, state::gameplay::entity_identity::kSourceCapacity>
        damageEntitySources{};
    /** Entities whose pools were published, each under its source; they reset with it. */
    std::array<EntityDamageObservation, kSquadObservationCapacity> entitySourceDamageObservations{};
    std::uint64_t nextEntityDamageTick{};
    lua_vm::ProgramIdentity identity{};
    mission_state::ProgramKey programKey{};
    std::array<char, 16> lastVmStage{};
    std::array<char, 32> lastVmStatus{};
    std::uint64_t eventsSeen{};
    std::uint64_t eventsCommitted{};
    std::uint64_t intentsTransportStaged{};
    std::uint64_t lastLoggedRevision{};
    std::uint64_t lastMissionSequence{};
    std::uint64_t missionStateRevision{};
    std::uint32_t missionPhase{};
    std::uint64_t activityStateRevision{};
    std::uint64_t durableIntentSequence{};
    std::uint64_t durableHostOutputRevision{};
    std::uint64_t expectedScriptableRevision{};
    /** Lease revision whose region move was armed, so a pending selection moves players once. */
    std::uint64_t armedStateRevision{};
    std::uint64_t deliveryDeadline{};
    std::uint64_t firstIntentAttempt{};
    host::Event pendingTimerEvent{};
    std::array<TriggerOccupancy, kTriggerOccupancyCapacity> triggerOccupancy{};
    std::array<SquadObservation, kSquadObservationCapacity> squadObservations{};
    /**
     * The levels each other link reported for a squad it does not simulate. Squad Sense restates
     * only the fields that moved, so a re-pin takes over the new link's own complete levels.
     */
    std::array<SquadObservation, kSquadObservationCapacity> squadShadows{};
    /** Set once a shadow found no row, so its warning is logged once until a row frees. */
    bool squadShadowAtCapacity{};
    std::array<GhostObservation, kGhostObservationCapacity> ghostObservations{};
    std::array<DamageObservation, kDamageObservationCapacity> damageObservations{};
    std::array<CombatantDamageObservation, kSquadObservationCapacity> combatantDamageObservations{};
    /** Damage levels replicated by authored entities, kept apart from the Type-2 Sense rows. */
    std::array<CombatantDamageObservation, kSquadObservationCapacity> entityDamageObservations{};
    std::array<DeviceObservation, kDeviceObservationCapacity> deviceObservations{};
    std::array<ObjectInteractionObservation, kObjectInteractionObservationCapacity>
        objectInteractionObservations{};
    /** Object state is world state, so it keeps one history: the link that simulates the object. */
    std::array<ObjectInteractionObservation, kObjectInteractionObservationCapacity>
        objectStateObservations{};
    std::array<ActorPathObservation, kActorPathObservationCapacity> actorPathObservations{};
    std::array<SceneObservation, kSceneObservationCapacity> sceneObservations{};
    std::array<ObjectiveObservation, kObjectiveObservationCapacity> objectiveObservations{};
    // The table is exactly as large as the session table, so it can never overflow.
    std::array<SessionRosterWatch, state::activity::kSessionCapacity> sessionRoster{};
    std::vector<MemberInputState> memberInputs{};
    bool partySnapshotValid{};
    /** Last published party life counts. Published once, then only on a change. */
    FireteamLife lastFireteamLife{};
    bool fireteamLifePublished{};
    /** Dynamically sized host-state reports waiting for this script, in arrival order. */
    std::vector<host::Event> scriptEvents{};
    std::uint32_t intentAttempts{};
    std::uint32_t startAttempts{};
    std::size_t durablePendingIntentCount{};
    std::size_t scriptEventRead{};
    std::uint16_t lastIntentStatus{(std::numeric_limits<std::uint16_t>::max)()};
    std::int32_t initialStateRegion{-1};
    /** Last client-reported activity region used to select state-local incoming references. */
    std::int32_t activeRegion{-1};
    ProgramStatus programStatus{ProgramStatus::none};
    DeliveryStage deliveryStage{DeliveryStage::idle};
    /** The player key the bound link's message 5 binds, read at attach. */
    std::uint64_t playerKey{};
    bool publicTarget{};
    bool missionStateBound{};
    bool missionStarted{};
    bool missionStateFaulted{};
    bool initialStateDeclared{};
    bool initialStateSelected{};
    bool startPending{};
    bool timerPending{};
    /** Set after the first roster read, so an attach never replays every existing peer. */
    bool sessionRosterObserved{};
    /** This VM reattached to its current session, so its entry is on_load, not on_start. */
    bool missionReattached{};
    bool occupied{};
};

/** Native Sense levels cannot cross an attempt or ActivityClient generation boundary. */
void clear_sense_levels(RuntimeInstance& instance) noexcept;

/**
 * Re-baselines the levels a new attempt must not inherit but the fallback arrays do not hold:
 * each member's live trigger, Ghost and interaction levels, and the channel-2 entity levels.
 * Player life is kept, because the client never resends an unchanged region or life field.
 */
inline void reset_member_observations(RuntimeInstance& instance) noexcept {
    for (MemberInputState& member : instance.memberInputs) {
        member.triggerOccupancy.fill({});
        member.ghostObservations.fill({});
        member.objectInteractionObservations.fill({});
    }
    instance.entitySourceDamageObservations.fill({});
    instance.damageEntitySources.fill({});
    instance.nextEntityDamageTick = 0;
}

/** Withdraws a retired source's observation history without changing mission or member state. */
inline void reset_shared_observations(RuntimeInstance& instance) noexcept {
    clear_sense_levels(instance);
    instance.entitySourceDamageObservations.fill({});
    instance.damageEntitySources.fill({});
    instance.nextEntityDamageTick = 0;
    instance.activeRegion = -1;
}

/**
 * The program's view through another joined link of its binding. The host applies an output in the
 * bubble that owns its target, and the link whose client holds that bubble carries it; the
 * program's route is only one such link.
 */
[[nodiscard]] inline sdk::BoundView routed_view(const RuntimeInstance& instance,
                                                std::uint64_t generation) noexcept {
    sdk::BoundView view = instance.view;
    view.activityClientGeneration = generation;
    return view;
}

/** @return The route and every joined private link this service pass read. */
[[nodiscard]] inline std::span<const std::uint64_t>
live_links(const RuntimeInstance& instance) noexcept {
    return std::span(instance.liveLinks).first(instance.liveLinkCount);
}

/** @return The pins this pass copied. */
[[nodiscard]] inline std::span<const host::ScriptablePin>
pass_pins(const RuntimeInstance& instance) noexcept {
    return std::span(instance.pins).first(instance.pinCount);
}

/** @return The link that simulates one target, read from this pass's pins and live links. */
[[nodiscard]] inline std::uint64_t simulating_link(const RuntimeInstance& instance,
                                                   const OutputClientRef& ref) noexcept {
    return admitted_generation(
        pass_pins(instance), ref, instance.view.activityClientGeneration, live_links(instance));
}

/** @return The ClientRef one Sense report names. */
[[nodiscard]] inline OutputClientRef
report_client_ref(const host::SenseObservationKey& key) noexcept {
    return {.registryKey = key.registryKey, .slotIndex = key.slotIndex, .slotType = key.slotType};
}

/**
 * Admits one shared simulation report against the pins copied for this pass.
 * @return True when the report came from the link that simulates its target.
 */
[[nodiscard]] inline bool admitted_report(const RuntimeInstance& instance,
                                          const host::SenseObservationKey& key,
                                          std::uint64_t sourceGeneration) noexcept {
    return sourceGeneration != 0
           && sourceGeneration == simulating_link(instance, report_client_ref(key));
}

/**
 * The actor-command policy row is this program's one registration for a squad, made on the link
 * the command was carried by, so a provocation from any link that can still carry the program's
 * output is the squad's own. The lane latches the squad and never reports it again.
 * @param sourceGeneration ActivityClient generation that owns the policy.
 * @return True when the provocation is admitted.
 */
[[nodiscard]] inline bool admitted_provocation(const RuntimeInstance& instance,
                                               std::uint64_t sourceGeneration) noexcept {
    const auto live = live_links(instance);
    return sourceGeneration != 0
           && (sourceGeneration == instance.view.activityClientGeneration
               || std::find(live.begin(), live.end(), sourceGeneration) != live.end());
}

/**
 * Hands a retained row to the link whose report was admitted. Another link's levels are not this
 * link's, so a rejoin or re-delivery that re-pins the target starts the row over as a first
 * sighting instead of comparing against them.
 * @param allocatedUsed The used flag a newly allocated row of this table carries.
 */
template <typename Row>
void claim_row(Row& row, std::uint64_t sourceGeneration, bool allocatedUsed) noexcept {
    if (row.sourceGeneration == sourceGeneration) {
        return;
    }
    Row fresh{};
    fresh.sourceGeneration = sourceGeneration;
    fresh.registryKey = row.registryKey;
    fresh.objectTag = row.objectTag;
    fresh.slotIndex = row.slotIndex;
    fresh.used = allocatedUsed;
    row = fresh;
}

/** Writes one bounded mission-script diagnostic line. Fields are key=value, error is free text. */
void log_line(core::log::Level level,
              const RuntimeInstance* instance,
              std::string_view stage,
              std::string_view result,
              std::string_view fields = {},
              std::string_view error = {}) noexcept;
/** Appends one host-state event for the script. */
void push_script_event(RuntimeInstance& instance, const host::Event& event) noexcept;
/** Raises a fireteam event on each private instance whose party life counts changed. */
void publish_fireteam_life(std::uint64_t now) noexcept;
/** Re-reads the exact party so every current member owns an input state before it is used. */
void refresh_mission_members(RuntimeInstance& instance) noexcept;
/** @return The input state of the member this client generation belongs to, or null. */
[[nodiscard]] MemberInputState* member_input(RuntimeInstance& instance,
                                             std::uint64_t generation) noexcept;
/** Merges the type-13 participation records of one Sense snapshot into the instance. */
void observe_player_life(RuntimeInstance& instance,
                         const host::SenseObservationSnapshot& sense) noexcept;

/** Raises one triggerState per report, and one edge per watched volume whose occupancy changed. */
void push_trigger_edges(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one dedicated type-31 edge from a decoded schema-0x8080879F msg-19 payload. */
void push_player_trigger(RuntimeInstance& instance, const host::Event& incident) noexcept;
/** Raises one exact Type-6 start/finish edge from a decoded schema-0x808087BF msg-19 payload. */
void push_cinematic(RuntimeInstance& instance, const host::Event& incident) noexcept;
/** Raises one event per named actor whose movement or delivery level changed. */
void push_actor_path_edges(RuntimeInstance& instance,
                           const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per damage monitor whose health, shield or revision changed. */
void push_damage_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept;
/** Raises the entity pools that changed since the last poll, each from its simulating link. */
void push_entity_damage_edges(RuntimeInstance& instance, std::uint64_t now) noexcept;
/**
 * Revalidates a queued entity damage event against its source, its allocation and the link that
 * simulates its combatant now. A refused copy is withheld again, so its link publishes it later.
 * @return True while the event may still reach the script.
 */
[[nodiscard]] bool current_entity_damage(RuntimeInstance& instance,
                                         const host::Event& event) noexcept;
/** Raises observed Type-2 damage pools and lifecycle resets without inferring damage. */
void push_combatant_damage_edges(RuntimeInstance& instance,
                                 const host::SenseObservationSnapshot& sense) noexcept;
/**
 * Raises a damage state from an entity's replicated damage levels (fractions of full), admitted
 * only for a key the entity poll does not publish and only from the link that simulates it.
 * @param sourceGeneration ActivityClient generation of the link that sent the levels.
 */
void push_entity_damage(RuntimeInstance& instance,
                        std::uint64_t sourceGeneration,
                        std::uint32_t registryKey,
                        std::uint8_t slotType,
                        std::uint16_t slotIndex,
                        float primary,
                        float secondary,
                        std::uint64_t tick) noexcept;
/** Raises current device values and sequence resets from accepted client reports. */
void push_device_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept;
/** Raises object state and accepted interaction events per interactable object. */
void push_object_interaction_edges(RuntimeInstance& instance,
                                   const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per Ghost link whose level changed. */
void push_ghost_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;
/** Hands the VM the retained Ghost-link levels, so a callback can read one it did not receive. */
void publish_ghost_levels(RuntimeInstance& instance) noexcept;
/** @return True when a Sense snapshot carries a report whose admission reads the pins. */
[[nodiscard]] bool reads_pins(const host::SenseObservationSnapshot& sense) noexcept;
/**
 * Reads the route and every joined private link of the binding into the instance.
 * @return True when the set differs from the one read before.
 */
[[nodiscard]] bool read_live_links(RuntimeInstance& instance) noexcept;
/**
 * Tells the VM which links' device requests it may read as applied: the live links last read. A
 * request pinned to a link that has left reads as not applied.
 * @return False when the list could not be retained.
 */
[[nodiscard]] bool publish_device_sources(const RuntimeInstance& instance, lua_vm::Vm& vm) noexcept;
/** Raises the squad state, spawn and death events derived from one msg 6 body. */
void push_squad_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;

/**
 * Finds the retained row for one slot, allocating a free row on a first observation.
 * @return Null when every row is in use.
 */
template <typename Row, std::size_t N>
[[nodiscard]] Row* find_slot_row(std::array<Row, N>& rows,
                                 const host::SenseObservationKey& key) noexcept {
    Row* spare = nullptr;
    for (Row& retained : rows) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
            return &retained;
        }
        if (!retained.used && spare == nullptr) {
            spare = &retained;
        }
    }
    if (spare != nullptr) {
        spare->used = true;
        spare->registryKey = key.registryKey;
        spare->objectTag = key.objectTag;
        spare->slotIndex = key.slotIndex;
    }
    return spare;
}

/**
 * Retains generation-qualified population facts before the derived squad callback.
 * Only the link that simulates the squad reports here, so the population follows that link.
 * @param moved The squad's levels were last read from another link, so the report moves the
 * population even when it restates no count.
 */
[[nodiscard]] bool observe_population(
    RuntimeInstance& instance,
    const host::SenseObservation& observation,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    bool moved) noexcept;
/** Raises one event per watched authored scene that latched complete. */
void push_scene_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept;
/** Raises one event per watched objective task counter that rose. */
void push_objective_edges(RuntimeInstance& instance,
                          const host::SenseObservationSnapshot& sense) noexcept;
/** Reports one committed phase change to the script. */
void queue_phase_entered(RuntimeInstance& instance, std::uint32_t previousPhase) noexcept;
/** Moves every private member to the region a freshly selected mission state belongs to. */
void arm_state_region_teleport(RuntimeInstance& instance,
                               const server::bap::ActivityMissionSeedPlan& plan) noexcept;
/** @return The identity every host-state edge that is not a Sense edge carries. */
[[nodiscard]] host::Event state_edge_event(const RuntimeInstance& instance) noexcept;
/** Raises one event per peer session that appeared or left this instance's destination. */
void push_session_roster_edges(RuntimeInstance& instance) noexcept;

// The runtime unit owns these. The delivery state machine calls into them.

/** Retires every queued mission event that belongs to one binding. */
void clear_pending_events(const state::activity::SessionBinding& binding) noexcept;
/** Copies one committed authoritative snapshot into the runtime's exact compare baseline. */
void accept_mission_state(RuntimeInstance& instance,
                          const mission_state::Snapshot& snapshot) noexcept;
/** Marks the already-faulted VM in durable State when its exact compare still matches. */
void persist_mission_fault(RuntimeInstance& instance) noexcept;
/** Faults both the VM and the exact server-owned mission record. */
void fault_instance(RuntimeInstance& instance, std::string_view reason) noexcept;

/**
 * Reads the pin bubbles of a view's bound catalog and scenario, which are all they depend on.
 * @param output Receives the table, empty for a view with no catalog.
 * @return False when the table could not be allocated.
 */
[[nodiscard]] bool bind_pin_bubbles(const sdk::BoundView& view,
                                    std::vector<PinBubbleRow>& output) noexcept;

/**
 * Moves each pin of a one-bubble target to the link holding that bubble's grant, so a target's
 * outputs and the admission of its reports name the client that simulates it. The retained body
 * itself stays as delivered: every private link's roster carries the whole estate.
 * @param pins Pins copied from the binding's retained bodies, rewritten in place.
 */
void follow_bubble_grants(const RuntimeInstance& instance,
                          std::span<host::ScriptablePin> pins) noexcept;

/**
 * Copies the pins of the binding's retained bodies for this pass. Sense is a sparse delta, so a
 * report that could not be admitted would leave its row behind for good; a failed copy faults.
 * @return False when the copy failed and the instance was faulted.
 */
[[nodiscard]] inline bool copy_pins(RuntimeInstance& instance) noexcept {
    instance.estateCopied = false;
    if (host::scriptable_auth_pins(instance.view.binding, instance.pins, instance.pinCount)) {
        follow_bubble_grants(instance, std::span(instance.pins).first(instance.pinCount));
        return true;
    }
    fault_instance(instance, "retained pin copy failed");
    return false;
}

/**
 * Copies the full retained bodies once for the pass the pins belong to.
 * @return False when the copy failed and the instance was faulted.
 */
[[nodiscard]] inline bool copy_estate(RuntimeInstance& instance) noexcept {
    if (instance.estateCopied) {
        return true;
    }
    if (!host::scriptable_auth_estate(
            instance.view.binding, instance.view.activityClientGeneration, instance.estate)) {
        fault_instance(instance, "retained estate copy failed");
        return false;
    }
    instance.estateCopied = true;
    return true;
}

// The delivery unit owns these. The runtime unit's service slice calls into them.

/** @return now plus delay, saturated at the maximum instead of wrapping. */
[[nodiscard]] std::uint64_t deadline_after(std::uint64_t now, std::uint64_t delay) noexcept;
/** Retires the head event after its single delivery attempt. */
void retire_script_event(RuntimeInstance& instance) noexcept;
/** Advances the delivery stage from one Host delivery lifecycle event. */
void observe_delivery_event(RuntimeInstance& instance,
                            const host::Event& event,
                            std::uint64_t now) noexcept;
/** Reconciles an output assigned before a terminal program fault. */
void reconcile_terminal_delivery(RuntimeInstance& instance) noexcept;

// The delivery unit owns these too. The intent fan-out drives the state machine through them.

/** Releases an exact unstaged Host revision while retaining the durable intent. */
[[nodiscard]] bool
release_delivery_state(RuntimeInstance& instance,
                       const mission_state::SquadPopulation* preparedParent = nullptr) noexcept;
/** Returns the instance to the idle stage and clears delivery timing. */
void clear_delivery(RuntimeInstance& instance) noexcept;
/** Retires one successfully applied local effect without assigning a Host output revision. */
[[nodiscard]] bool complete_local_effect(RuntimeInstance& instance,
                                         std::string_view result) noexcept;
/** Faults the program and abandons the delivery. */
void fault_delivery(RuntimeInstance& instance,
                    std::string_view result,
                    std::string_view reason) noexcept;
/** Reports one refused request to the script and keeps the program running. */
void refuse_delivery(RuntimeInstance& instance,
                     std::string_view result,
                     std::string_view reason,
                     host::EffectOutcome outcome) noexcept;
/** Dedup keys for report_intent_status. Each pending wait owns one value in its own range. */
inline constexpr std::uint16_t kIntentStatusCancelPending = 0x2FF;
inline constexpr std::uint16_t kIntentStatusSceneOutputBusy = 0x301;
inline constexpr std::uint16_t kIntentStatusStateTransitionPending = 0x302;
inline constexpr std::uint16_t kIntentStatusSceneLeasePending = 0x303;
/** Logs one adapter status, and only when it differs from the last one logged. */
void report_intent_status(RuntimeInstance& instance,
                          std::uint16_t status,
                          std::string_view name) noexcept;
/** @return Whether the intent has been in delivery longer than its lifetime allows. */
[[nodiscard]] bool intent_lifetime_expired(const RuntimeInstance& instance,
                                           std::uint64_t now) noexcept;
/** @return Whether an exact transport stage was found, so the delivery completed here. */
[[nodiscard]] bool reconcile_transport_stage(RuntimeInstance& instance) noexcept;
/** @return Whether an expired delivery was completed or left owned, so the caller must stop. */
[[nodiscard]] bool reconcile_expired_delivery(RuntimeInstance& instance) noexcept;
/** @return Whether a stage deadline fired, so no further delivery work is owed this tick. */
[[nodiscard]] bool service_delivery_timeout(RuntimeInstance& instance, std::uint64_t now) noexcept;

// The dispatch unit owns this. The runtime unit's service slice calls it.

/** Resolves a durable SDK actor command through the installed native policy. */
[[nodiscard]] ActorCommandPolicyStatus dispatch_actor_command(const RuntimeInstance& instance,
                                                              const lua_vm::Intent& intent,
                                                              std::uint64_t generation) noexcept;

/** Raises one queued intent, or advances the delivery already in flight. */
void dispatch_intent(RuntimeInstance& instance, std::uint64_t now) noexcept;

// The runtime unit owns the instance table too. Every other unit reaches it through these.

extern std::array<RuntimeInstance, host::kInstanceCapacity> g_instances;

/** @return The open slot for one exact binding, or null. */
[[nodiscard]] RuntimeInstance*
find_instance(const state::activity::SessionBinding& binding) noexcept;
/** @return One unused slot, or null when every slot is open. */
[[nodiscard]] RuntimeInstance* free_instance() noexcept;
/** Frees one slot; its queued events are retired unless the caller keeps them for a reattach. */
void clear_instance(RuntimeInstance& instance, bool clearPending = true) noexcept;
/** Retains the last VM stage and status shown on the panel. */
void note_vm_status(RuntimeInstance& instance,
                    std::string_view stage,
                    std::string_view status) noexcept;
/** Commits the VM's phase/revision/start transaction into exact server-owned State. */
[[nodiscard]] bool commit_mission_state(RuntimeInstance& instance,
                                        bool started,
                                        std::uint64_t nextInputSequence) noexcept;

// The sources unit owns these: where a program's script and its require path come from.

/** How reading one mission script ended. */
enum class SourceStatus : std::uint8_t {
    ready,
    missing,
    fileError,
    tooLarge,
};

/** Resolves the script root and the SDK Lua search path. Logs its own refusal. */
[[nodiscard]] bool resolve_script_paths() noexcept;
/**
 * Reads the activity's script: a loaded mod's, then the authored one, then the generated one.
 * @param output Receives the source; it stays valid until the next read.
 * @param modId Receives the supplying mod's id, or an empty string for a base script.
 * @return Why no source was read, or ready.
 */
[[nodiscard]] SourceStatus read_source(const sdk::Catalog& catalog,
                                       const format::Activity& activity,
                                       std::span<const char>& output,
                                       std::array<char, state::mods::kIdCapacity>& modId) noexcept;
/**
 * Appends `<mods>\<folder>` for one mod.
 * @return False when the path does not fit.
 */
[[nodiscard]] bool append_mod_directory(core::path::Buffer& path,
                                        const state::mods::View& view,
                                        const state::mods::Record& record) noexcept;
/** @return The Lua require path every program opens with. */
[[nodiscard]] const std::array<char, lua_vm::kSearchPathByteCapacity>&
sdk_lua_search_path() noexcept;
/** Zeroes the bytes `read_source` returned, once the program no longer needs them. */
void clear_source(std::span<const char> source) noexcept;
/** Clears the script buffer and both paths. */
void clear_script_sources() noexcept;
/** Resolves a loaded filename to its loaded mod owner; empty output means a base file. */
[[nodiscard]] bool module_source_owner(std::string_view path, std::span<char> modId) noexcept;

// The attach unit owns these. The service slice and the lifecycle entry points call into them.

/** Drops slots that no longer match, publishes the roster, and attaches active host instances. */
void synchronize_instances(std::uint64_t now) noexcept;
/** Advances fresh programs only when their declared state roster has reached transport output. */
void service_pending_starts(std::uint64_t now) noexcept;

// The diagnostics unit owns these. The attach unit and the panel snapshot call into them.

/** @return Stable panel-facing class for one retained attach result. */
[[nodiscard]] const char* attach_result_name(AttachResult value) noexcept;
/**
 * Reports an attach result when it changes for the binding.
 * @param name Stable result token written to the log.
 * @param activityRow Zero-based SDK row, or the absent sentinel before one resolves.
 */
void report_attach_result(
    const state::activity::SessionBinding& binding,
    AttachResult result,
    std::string_view name,
    std::uint32_t activityRow = format::kAbsentIndex,
    sdk::Status sdkStatus = sdk::Status::notReady,
    generated::BindStatus generatedWorldStatus = generated::BindStatus::invalidBoundView) noexcept;
/** @return True when the Host still reports this binding as active. */
[[nodiscard]] bool is_active(const host::DiagnosticsSnapshot& diagnostics,
                             const state::activity::SessionBinding& binding) noexcept;
/** Drops results after their exact bindings leave the active Host set. */
void retire_attach_diagnostics(const host::DiagnosticsSnapshot& diagnostics) noexcept;
/** Clears every retained attach result. */
void clear_attach_diagnostics() noexcept;
/** Copies one instance and its VM counters into the panel-facing diagnostics row. */
void copy_diagnostics(const RuntimeInstance& instance, InstanceDiagnostics& output) noexcept;
/** Copies every retained attach result into the panel-facing snapshot. */
void snapshot_attach_diagnostics(DiagnosticsSnapshot& output) noexcept;

// The reload unit owns these. The service slice and shutdown call into them.

/** Reopens the programs the mailbox request names once each is at a safe point. */
void service_reload(std::uint64_t now) noexcept;
/** Drops any unanswered request and its answer. */
void clear_reload_mailbox() noexcept;
/** Copies the committed attempt, revision, cursors, phase and intent head into the instance. */
void accept_compare_fields(RuntimeInstance& instance,
                           const mission_state::Snapshot& snapshot) noexcept;
/** Opens replacement code against the same exact view without changing the running instance. */
[[nodiscard]] lua_vm::OpenStatus prepare_reload_program(const RuntimeInstance& instance,
                                                        std::span<const char> source,
                                                        std::string_view modId,
                                                        lua_vm::Vm& vm,
                                                        lua_vm::ProgramIdentity& identity,
                                                        mission_state::ProgramKey& key) noexcept;
/** Retargets a prepared VM only while its SDK, world and source identities still match. */
[[nodiscard]] bool rebind_reload_program(const RuntimeInstance& instance,
                                         std::span<const char> source,
                                         lua_vm::Vm& vm,
                                         lua_vm::ProgramIdentity& identity,
                                         const mission_state::ProgramKey& key) noexcept;
/** @return True when mission scripting is on and its paths resolved. Takes the runtime lock. */
[[nodiscard]] bool runtime_ready() noexcept;

// The feed unit owns these. The service slice and the lifecycle entry points call into them.

/** Retires every queued mission event of every binding and frees the queue. */
void clear_all_pending_events() noexcept;
/** Keeps accepted values but removes every VM- and ActivityClient-generation-local decision. */
void reset_pending_events_for_reattach(const state::activity::SessionBinding& binding) noexcept;
/** Retires rows as soon as authoritative State no longer owns their exact binding. */
void retire_unbound_pending_events() noexcept;
/** @return Queued rows still owed to one exact binding. */
[[nodiscard]] std::size_t
pending_event_count(const state::activity::SessionBinding& binding) noexcept;
/** @return True when one queued accepted row is still owed to this instance. */
[[nodiscard]] bool has_pending_host_input(const RuntimeInstance& instance) noexcept;
/** Points both Host cursors at the current feed heads and replays retained accepted inputs. */
void reset_feed_cursors() noexcept;
/** Clears both Host cursors. */
void clear_feed_cursors() noexcept;
/** Reads new host events, advances delivery, and queues only the lifecycle rows for scripts. */
void consume_delivery_events(std::uint64_t now) noexcept;
/** Reads the ordered mission-input feed and drains the queue. */
void consume_mission_inputs(std::uint64_t now) noexcept;
/**
 * Runs one event through the VM, commits what it changed, and faults on a script failure.
 * @param sense Values owned by a Sense row, or null.
 * @param clientMessage Envelope snapshot owned by a client-message row, or null.
 * @return The VM call status; `inactive` when no callback could take the event.
 */
[[nodiscard]] lua_vm::CallStatus dispatch_event(RuntimeInstance& instance,
                                                const host::Event& event,
                                                const host::SenseObservationSnapshot* sense,
                                                const host::ClientMessageSnapshot* clientMessage,
                                                std::uint64_t now) noexcept;

} // namespace sunrise::server::activity::mission
