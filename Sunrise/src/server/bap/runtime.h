#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../client/network/consumer.h"
#include "../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../state/activity/definition.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/runtime/state.h"
#include "../activity/host_runtime.h"
#include "activity_authority_query.h"
#include "activity_authority_reset.h"

namespace sunrise::server::bap {

/** Read-only eligibility state from one exact authenticated ActivityClient link. */
struct ActivityLinkView final {
    std::size_t matchingLinks{};
    std::uint64_t activityClientGeneration{};
    std::int32_t effectiveRegion{-1};
    /** Exact package slice set retained from the client's D6 teleport state. */
    std::int32_t sliceSetIndex{-1};
    /** Destination arrival slice committed before the client can send D6. */
    std::int32_t arrivalSliceSetIndex{-1};
    /** True when the region is live connection state rather than an arrival fallback. */
    bool effectiveRegionReported{};
    /** The private ActivityClient completed its entity-slot join for this exact generation. */
    bool joined{};
    bool publicTarget{};
    /** Last roster-build outcome reported on this link, for atomic diagnostics. */
    std::uint8_t rosterReason{};
    /** The player key this link's message 5 binds, which the client matches on its own datum. */
    std::uint64_t playerKey{};
};

/** The active ActivityClient selected for a local world slice. */
struct CurrentActivityLinkView final {
    state::activity::SessionBinding binding{};
    std::uint64_t activityClientGeneration{};
    std::size_t activeLinks{};
    std::size_t matchingRegions{};
    std::int32_t effectiveRegion{-1};
    bool publicTarget{};
};

/** Resolves the player's region host for world actions, including public citizen hosts. */
[[nodiscard]] bool current_activity_host_link_view(std::int32_t localSliceSet,
                                                   CurrentActivityLinkView& output) noexcept;

/** Common-root inputs owned by one exact ActivityClient generation. */
struct ActivityReplicationView final {
    state::activity::SessionBinding binding{};
    middleware::bap::activity_message::patch_epoch::PatchEpoch patchEpoch{};
    std::uint64_t activityClientGeneration{};
    std::uint64_t groupSessionId{};
    std::uint64_t memberId{};
    std::uint8_t replicationEpoch{};
};

/** Exact generated Auth-mapping selection retained by one ActivityClient lease. */
struct ActivityMissionSeedPlan final {
    /** Objects the mission left out of this seed. Empty means the whole selected state. */
    std::array<state::activity_sdk::MissionSeedOmission,
               state::activity_sdk::kMissionSeedOmitCapacity>
        omissions{};
    std::uint32_t omissionCount{};
    std::uint32_t activityRow{0xFFFFFFFFU};
    std::uint32_t scenarioRow{0xFFFFFFFFU};
    std::uint32_t stateRow{0xFFFFFFFFU};
    std::uint32_t bubbleRow{0xFFFFFFFFU};
    std::uint32_t bubbleOrdinal{0xFFFFFFFFU};
    std::uint32_t stateOrdinal{};
    std::uint32_t entryIndex{};
    std::uint32_t sliceSetIndex{};
    std::uint32_t effectiveRegion{};
    std::uint32_t occurrenceCount{};
    std::uint32_t groupCount{};
    std::uint32_t authMappingSlots{};
    std::uint32_t authResetSlots{};
    std::uint32_t senseSuppressedSlots{};
};

/** Stable transport-side outcome for reading the automatic selected-state roster publication. */
enum class ActivityMissionSeedLeaseStatus : std::uint8_t {
    ready,
    noActivityLink,
    staleActivityClient,
    wrongScenario,
    missingLiveSliceSet,
    wrongSliceSet,
    outputBusy,
    refused,
};

/** Read-only state of one connection-scoped SDK selected-state roster lease. */
struct ActivityMissionSeedLeaseView final {
    ActivityMissionSeedPlan plan{};
    std::size_t matchingLinks{};
    std::uint64_t activityClientGeneration{};
    std::uint64_t revision{};
    std::uint64_t publishedRevision{};
    bool configured{};
    bool publicationPending{};
    /** True while the selection's publication deliberately waits for the client's arrival. */
    bool regionArrivalPending{};
};

/** Applies one connection-scoped BAP lifecycle event. */
[[nodiscard]] bool consume(const client::network::BapRequest& request,
                           client::network::BapResponse& response) noexcept;

/** Counts authenticated BAP links that currently own one exact activity generation. */
[[nodiscard]] std::size_t
activity_link_count(const state::activity::SessionBinding& binding) noexcept;

/**
 * Reads the unique matching ActivityClient and the exact region its msg-5 builder will use.
 * @param binding Exact Activity Host generation selected by the caller.
 * @param output Cleared, then receives the link count, nonzero generation, and connection fields.
 * @return True only when exactly one authenticated ActivityClient owns the binding.
 */
[[nodiscard]] bool activity_link_view(const state::activity::SessionBinding& binding,
                                      ActivityLinkView& output) noexcept;

/** Checks whether one exact ActivityClient can change its SDK selected-state roster lease. */
[[nodiscard]] ActivityMissionSeedLeaseStatus
activity_mission_seed_available(const state::activity::SessionBinding& binding,
                                std::uint32_t scenarioRow,
                                std::uint64_t expectedGeneration) noexcept;

/** Reads one exact ActivityClient's connection-scoped SDK selected-state roster lease. */
[[nodiscard]] ActivityMissionSeedLeaseStatus
activity_mission_seed_lease(const state::activity::SessionBinding& binding,
                            std::uint32_t scenarioRow,
                            std::uint64_t expectedGeneration,
                            ActivityMissionSeedLeaseView& output) noexcept;

/** Replaces one exact ActivityClient's selected-state roster lease with a materialized plan. */
[[nodiscard]] ActivityMissionSeedLeaseStatus
select_activity_mission_seed(const state::activity::SessionBinding& binding,
                             const ActivityMissionSeedPlan& plan,
                             std::uint64_t expectedGeneration) noexcept;

/** Read-only check of a canonical type-23 target on one live ActivityClient generation. */
[[nodiscard]] bool
activity_type23_override_available(const state::activity::SessionBinding& binding,
                                   const activity::host::ScriptableTarget& target,
                                   std::int32_t expectedRegion,
                                   std::uint64_t expectedGeneration) noexcept;

/**
 * Selects the live ActivityClient whose msg-5 region matches the client's local slice set.
 * One active link is accepted without a match. Multiple unmatched or multiply matched links are
 * ambiguous and return false instead of being ordered by their session ids.
 */
[[nodiscard]] bool current_activity_link_view(std::int32_t localSliceSet,
                                              CurrentActivityLinkView& output) noexcept;

/** Reads the exact ActivityClient inputs needed by the gameplay common root. */
[[nodiscard]] bool activity_replication_view(const state::activity::SessionBinding& binding,
                                             ActivityReplicationView& output) noexcept;

/** Reads the unique ActivityClient whose activity session is the supplied view token. */
[[nodiscard]] bool activity_replication_view_for_session(std::uint64_t activitySessionId,
                                                         ActivityReplicationView& output) noexcept;

/** Reads the unique ActivityClient bound to one gameplay group session. */
[[nodiscard]] bool activity_replication_view_for_group(std::uint64_t groupSessionId,
                                                       ActivityReplicationView& output) noexcept;

/** Queues activity message 44 on one exact ActivityClient generation. */
[[nodiscard]] bool request_replication_epoch(const state::activity::SessionBinding& binding,
                                             std::uint64_t expectedGeneration,
                                             std::uint8_t generation) noexcept;

/** Queues one msg-30 readback on an exact unique ActivityClient link. */
[[nodiscard]] ActivityAuthorityQueryStatus
request_activity_authority_query(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept;

/** Copies one complete connection-owned authority readback. */
[[nodiscard]] ActivityAuthorityQueryStatus
activity_authority_query_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityQuerySnapshot& output) noexcept;

/** Queues one msg-28 authority-mask rebuild on an exact unique ActivityClient link. */
[[nodiscard]] ActivityAuthorityResetStatus
request_activity_authority_reset(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept;

/** Copies one complete connection-owned authority-reset result. */
[[nodiscard]] ActivityAuthorityResetStatus
activity_authority_reset_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityResetSnapshot& output) noexcept;

/** Queues a type-23 override only while exactly one authenticated link owns the binding. */
[[nodiscard]] bool request_activity_type23_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
    float value,
    bool snap,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one activity lifetime change while exactly one authenticated link owns the binding. */
[[nodiscard]] bool request_activity_lifetime_override(
    const state::activity::SessionBinding& binding,
    std::uint8_t lifetimeState,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues a generated type-23 update while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_type23_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
    float value,
    bool snap,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one structurally compiled SDK Auth body through the durable message-5 lane. */
[[nodiscard]] bool request_activity_sdk_auth_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::byte> body,
    std::uint16_t bitCount,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues a type-31 pulse only while exactly one authenticated link owns the binding. */
[[nodiscard]] bool request_activity_type31_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    std::int32_t expectedRegion,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues a generated state-local type-31 pulse while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_type31_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored sequence restart only while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_sequence_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored cinematic start or stop while its mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_cinematic_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    bool active,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one type-42 performance start while its mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_performance_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint32_t stateNameHash,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored-scene activation only while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_authored_scene_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored dialogue line only while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_dialogue_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::uint16_t cueIndex,
    std::uint16_t authoredCueCount,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one objective reset only while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_objective_reset(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues one authored task only while its exact mission-seed state is live. */
[[nodiscard]] bool request_activity_state_local_task_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup& stateLocalRosterGroup,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    std::uint32_t scenarioRow,
    std::uint32_t stateRow,
    const activity::host::ScriptableOutputReservation* reservation = nullptr) noexcept;

/** Queues a squad placement only while exactly one authenticated link owns the binding. */
[[nodiscard]] bool request_activity_squad_override(
    const state::activity::SessionBinding& binding,
    const activity::host::ScriptableTarget& target,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup,
    std::span<const std::int32_t> requestedCounts,
    middleware::bap::activity_message::squad_auth::Mode mode,
    std::optional<std::uint32_t> nameHash,
    std::int32_t expectedRegion,
    std::uint64_t expectedGeneration,
    const activity::host::ScriptableOutputReservation* reservation = nullptr,
    std::array<std::int8_t, 4> authoredProfile = {}) noexcept;

/** Cancels one exact typed override revision while excluding activity-link publication. */
[[nodiscard]] bool
cancel_activity_scriptable_override(const state::activity::SessionBinding& binding,
                                    std::uint64_t expectedRevision) noexcept;

/** Cancels a pending raw incident while excluding activity-link publication. */
[[nodiscard]] bool
cancel_activity_host_incident(const state::activity::SessionBinding& binding) noexcept;

#if defined(SUNRISE_BAP_FRAME_TEST)
/**
 * Copies one connection's own send nonce and AES-GCM session key.
 * Test-only: a key export must not ship, so the production DLL compiles this out. The hello
 * generates this material per connection and never puts it in State, so a fixture has no other way.
 * @param connectionId Nonzero connection slot.
 * @param sendNonce Gets the server-direction nonce this connection will seal with next.
 * @param sessionKey Gets this connection's own key.
 * @return True only when the slot is open and its server hello has armed the channel.
 */
[[nodiscard]] bool session_channel(std::uint32_t connectionId,
                                   std::array<std::byte, state::kBapNonceSize>& sendNonce,
                                   std::array<std::byte, state::kAesKeySize>& sessionKey) noexcept;
#endif

/** Wipes every connection-owned nonce and transform buffer. */
void shutdown() noexcept;

} // namespace sunrise::server::bap
