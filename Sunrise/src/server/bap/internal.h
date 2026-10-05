#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../client/network/consumer.h"
#include "../../core/threading/srw_lock.h"
#include "../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../middleware/bap/activity_message/replicate_membership.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/activity_message/transport_report.h"
#include "../../middleware/bap/frame.h"
#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity/bubble_authority/definition.h"
#include "../../state/activity/definition.h"
#include "../../state/activity/mission/definition.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/build_data/scenarios/definition.h"
#include "../../state/gameplay/external/squad_entity_retirement.h"
#include "../../state/runtime/runtime.h"
#include "../../state/social/steam_roster.h"
#include "../activity/host_runtime.h"
#include "activity_authority_query_owner.h"
#include "activity_authority_reset_owner.h"
#include "activity_link_selection.h"
#include "encrypted/activity_host_manager/activity_startup_reservations.h"
#include "encrypted/queuez/definition.h"
#include "encrypted/queuez/public_subscriptions.h"
#include "nat_relay_service.h"
#include "runtime.h"

namespace sunrise::server::bap {

/** One session per transport peer slot, so a connection id indexes this array directly. */
inline constexpr std::size_t kSessionCount = client::network::kBapConnectionCount;
static_assert(state::matchmaking::kContextCapacity >= kSessionCount);
/**
 * A delivered activity frame defers the next silence-prevention write.
 * This is the host's own period. It is not the floor handed to the client, which is a separate
 * value, and no bulk body may be scheduled on it.
 */
inline constexpr std::uint64_t kActivityKeepaliveIntervalMs = 2'000;

/** Counts matching authenticated links while the caller already owns the BAP lock. */
[[nodiscard]] std::size_t
activity_link_count_locked(const state::activity::SessionBinding& binding) noexcept;
/** Counts the exact authenticated recipient generation for read-only content projection. */
[[nodiscard]] std::size_t activity_link_count_locked(const state::activity::SessionBinding& binding,
                                                     std::uint64_t recipientGeneration) noexcept;

// Top-level groups the client can hold; the largest installed scenario has 78 in every state.
inline constexpr std::size_t kScenarioWideGroupCapacity =
    middleware::bap::activity_message::sensor_auth_update::kClientGroupCapacity;

/** Fixed scratch storage owned by the lock, kept off the Client thread's stack. */
struct Scratch {
    /** Reused account image and distinct decode staging; callers hold the BAP session lock. */
    state::AccountState accountImage{};
    state::AccountState accountDecode{};
    /** Copy the live directory before mutation and commit only after the reply fits. */
    state::social::Hub socialDirectory{};
    std::array<std::byte, client::network::kBapFrameCapacity> plaintext{};
    std::array<std::byte, client::network::kBapFrameCapacity> responseBody{};
    std::array<std::byte, client::network::kBapFrameCapacity> responsePayload{};
    std::array<std::byte, client::network::kBapFrameCapacity> sealed{};
    std::array<std::byte, client::network::kBapFrameCapacity> framed{};
    /** Roster groups the outbound body's slot spans point into, top-level and per-bubble alike. */
    std::array<state::build_data::scenarios::RosterGroup,
               middleware::bap::activity_message::sensor_auth_update::kPublishedGroupCapacity>
        rosterGroups{};
    /** One group held aside while the appended groups are put into first-seen order. */
    state::build_data::scenarios::RosterGroup rosterGroupSpare{};
    /** Groups of the objects present in every state of the bound scenario. */
    std::array<state::build_data::scenarios::RosterGroup, kScenarioWideGroupCapacity>
        rosterWideGroups{};
    /** Exact typed auth bodies the outbound snapshot span points into. */
    std::array<middleware::bap::activity_message::sensor_auth_update::AuthOverride,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        rosterAuthOverrides{};
    /** Decoded device channels from the exact Auth rows written by the current roster. */
    std::array<state::activity::mission::DevicePublication,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        rosterDevicePublications{};
    std::array<middleware::bap::activity_message::sensor_auth_update::SenseOverride,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        rosterSenseOverrides{};
    /** SDK-authored scene inputs staged before their exact msg-5 targets are installed. */
    std::array<state::activity_sdk::AuthoredSceneSeed,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        rosterSceneSeeds{};
    /** Per-bubble sub-blocks the outbound body's field-1 span points into. */
    std::array<middleware::bap::activity_message::sensor_auth_update::BubbleSubBlock,
               state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlocks{};
    /** Keys each sub-block carries, which its own span points into. */
    std::array<
        std::array<std::uint32_t,
                   middleware::bap::activity_message::sensor_auth_update::kBubbleKeyCapacity>,
        state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlockKeys{};
};

/** One registry key and its package object tag copied from an exact msg-5 roster snapshot. */
struct RosterDecodeEntry final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
};

/** Complete msg-5 roster identity map retained for one exact ActivityClient generation. */
struct RosterDecodeMap final {
    std::array<RosterDecodeEntry,
               middleware::bap::activity_message::sensor_auth_update::kPublishedGroupCapacity>
        entries{};
    std::uint64_t bindingGeneration{};
    std::uint16_t count{};
    bool valid{};
};

/**
 * One published group's registration identity and the revision last sent beside its key.
 * The wire carries one revision byte per key. The client tears down and rebuilds a group's
 * objects only when that byte moves, so the byte stays put while the group's identity holds.
 */
struct RosterGroupLease {
    std::uint32_t key{};
    std::uint32_t identityFold{};
    /** Bubbles this key was ever registered under; a later push keeps it in every one of them. */
    std::uint64_t bubbles{};
    std::uint8_t sequence{};
    bool used{};
};

/** Lease slots mirror the roster's published-group capacity. */
inline constexpr std::size_t kRosterGroupLeaseCapacity =
    middleware::bap::activity_message::sensor_auth_update::kPublishedGroupCapacity;

/**
 * What one staged roster body owes State, and the counters to put back if it is discarded.
 * A bubble is offered once, and each group revision may rebuild that registry key. Counters and
 * staged per-group revisions may move only once the frame reaches the caller.
 */
struct RosterPublication {
    state::activity::bubble_authority::Grant grant{};
    state::gameplay::squad_entity_retirement::RetirementPlan entityRetirement{};
    /** Epochs remain staged until both retirement and roster frames reach the caller. */
    std::uint64_t retirementSequence{};
    std::uint8_t retirementPriorEpoch{};
    std::uint8_t retirementBaseEpoch{};
    std::uint8_t retirementEpoch{};
    bool priorRosterOwedForEpoch{};
    /** Exact decode identities carried by this staged complete roster snapshot. */
    RosterDecodeMap decodeMap{};
    /** These proofs become report eligibility only after the frame reaches the caller. */
    std::array<state::activity::mission::DevicePublication,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        devicePublications{};
    state::activity::mission::DevicePublicationBoundary devicePublicationBoundary{};
    state::activity::SessionBinding devicePublicationBinding{};
    std::uint16_t devicePublicationCount{};
    /** Exact typed body carried by this staged roster, if any. */
    activity::host::PendingScriptableOverride scriptableOverride{};
    /** ActivityClient generation that staged this grant and its roster counters. */
    std::uint64_t bindingGeneration{};
    /** Group leases as they stood before this body, put back if it never reaches the caller. */
    std::array<RosterGroupLease, kRosterGroupLeaseCapacity> priorLeases{};
    std::uint8_t priorSends{};
    std::uint8_t priorState{};
    /** Region epoch and the bubble it was stamped for, put back with the leases above. */
    std::uint8_t priorRegionEpoch{};
    std::int32_t priorRegionBubble{-1};
    /** Generated squad-group revision carried by this staged body. */
    std::uint8_t squadStateSequence{};
    /** Activity Host state revision carried by this body. */
    std::uint64_t hostStateRevision{};
    /** SDK selected-state roster lease revision carried by this body. */
    std::uint64_t missionSeedRevision{};
    /** Type-17 lifetime state carried for that revision. */
    std::uint8_t hostLifetimeState{};
    /** Exact authored region that owns the staged state-local group. */
    std::int32_t stateLocalRegion{-1};
    /** Set when the staged body carried a bubble grant that State has not recorded yet. */
    bool hasGrant{};
    /** Set when this body carries a pending Activity Host state revision. */
    bool hasHostState{};
    /** Set when this body carries a not-yet-published SDK selected-state roster lease revision. */
    bool hasMissionSeedRevision{};
    /** Set when this body carries the pending typed body above. */
    bool hasScriptableOverride{};
    /** Set when the delivered body merges into this binding's retained squad Auth set. */
    bool activatesSquadOverride{};
    /** Set when the staged squad body carries its own per-group revision. */
    bool hasSquadStateSequence{};
    /** Set when this body is the leave delta answering activity msg 15. */
    bool peerLeave{};
    /** Set when this body carries the team wait bit, built before the client's arrival report. */
    bool awaitClientSync{};
    /** Set while a roster body is staged and its outcome is undecided. */
    bool staged{};
};

/** Compact retained squad body; shared target fields and the generated group live on its group. */
struct RetainedSquadAuth {
    std::array<std::byte, middleware::bap::activity_message::squad_auth::kMaximumRetainedByteCount>
        body{};
    std::uint32_t generation{};
    std::uint16_t rosterSlotOffset{};
    std::uint16_t slotIndex{};
    std::uint16_t bitCount{};
    std::uint16_t byteCount{};
    /** Dense retained-group index that owns this body. */
    std::uint8_t groupIndex{};
};

/** One generated registry key and the squad Auth bodies that target its slots. */
struct RetainedSquadGroup {
    /** Fields shared by this group's retained slots; only slot offset and index vary. */
    activity::host::ScriptableTarget scopeTarget{};
    /** Exact generated SDK group shared by this group's retained slots. */
    state::build_data::scenarios::RosterGroup stateLocalRosterGroup{};
    std::int32_t region{-1};
    std::uint16_t authCount{};
    /** Last delivered revision of the generated state-local roster group. */
    std::uint8_t stateSequence{};
    /** Region epoch the revision was armed under; a newer one re-arms the group. */
    std::uint8_t regionEpoch{};
};

/** All generated squad groups retained by one exact ActivityClient binding. */
struct SquadOverrideLease {
    std::array<RetainedSquadAuth,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity>
        authBodies{};
    std::array<RetainedSquadGroup,
               middleware::bap::activity_message::sensor_auth_update::kPublishedGroupCapacity>
        groups{};
    std::uint64_t bindingGeneration{};
    std::uint16_t authCount{};
    std::uint16_t groupCount{};
    bool active{};
};

/** Off-by-default generated selected-state roster plan pinned to one exact ActivityClient
 * generation. */
struct MissionSeedLease {
    ActivityMissionSeedPlan plan{};
    std::uint64_t bindingGeneration{};
    std::uint64_t revision{};
    std::uint64_t publishedRevision{};
    /**
     * Every authored region this lease has selected, in selection order. The peer's registered set
     * only grows: a group is torn down by a changed state byte, never by being left out, and every
     * message must seed its sync records. So each publication carries the union of these regions.
     */
    std::array<std::uint32_t, middleware::content::packages::tables::kSliceSetIndexFactor>
        registeredRegions{};
    std::uint8_t registeredRegionCount{};
    bool configured{};
    /**
     * Set once the complete selected-state set has been published. It only ratchets: shrinking
     * the published set back to the transition subset would re-register the shared groups.
     */
    bool fullSetPublished{};
    /**
     * The plan the previous selection held, kept while a region change waits for the client to
     * arrive. Publications in that window answer this plan's region: the client is tearing its
     * old world down, and registering the new region's groups into it races the teardown.
     */
    ActivityMissionSeedPlan previousPlan{};
    /** True from a region-changing selection until the client's post-arrival solicited answer. */
    bool regionArrivalPending{};
    /** Set when a mission script selected the plan. An adopted default plan is not a selection. */
    bool scriptSelected{};
};

static_assert(middleware::bap::activity_message::sensor_auth_update::kAuthOverrideCapacity
              == state::build_data::scenarios::kRosterSlotCapacity);

/** ActivityClient role owned by one authenticated BAP link. */
enum class ActivityClientRole : std::uint8_t {
    none,
    privateCurrent,
    publicTarget,
};

/** Exact activity-session generations owned by one BAP link. */
struct ActivityClientBinding {
    /** Target/current session that every activity envelope on this link names. */
    state::activity::SessionBinding session{};
    /** Same as session for private links; advertised source for public targets. */
    state::activity::SessionBinding source{};
    std::uint64_t groupSessionId{};
    std::uint64_t hostGeneration{};
    /** Changes on every bind and rejoin, even when the session id stays the same. */
    std::uint64_t bindingGeneration{};
    /** Last sequence queued on this ordered connection, not proof of client application. */
    std::uint64_t replicationSequence{};
    /** Low byte of the last sequence queued on this connection. */
    std::uint8_t replicationEpoch{};
    /** Private: last citizen region. Public: immutable region captured by the host binding. */
    std::int32_t advertisedRegion{-1};
    /** True when the last advertisement named a private region's own Bubble Host. */
    bool advertisedPrivate{};
    ActivityClientRole role{ActivityClientRole::none};
};

/** Patch epoch tied to the exact ActivityClient binding that received it. */
struct BoundPatchEpoch {
    middleware::bap::activity_message::patch_epoch::PatchEpoch value{};
    std::uint64_t bindingGeneration{};
    bool seen{};
};

/** Host rows one membership body's directory holds against replacement. */
struct AdvertisementRetains {
    std::array<std::uint64_t,
               middleware::bap::activity_message::replicate_membership::kCitizenCapacity>
        hostGenerations{};
    std::uint8_t count{};
};

/** Host-session retains staged by one membership body until its frame is published. */
struct AdvertisementPublication {
    AdvertisementRetains retains{};
    bool staged{};
};

/** One staged msg-19 host output, committed only after its complete frame reaches the caller. */
struct IncidentPublication {
    std::uint64_t bindingGeneration{};
    std::uint64_t revision{};
    bool staged{};
};

/** One generation-bound activity message 44 request. */
struct ReplicationEpochPublication {
    std::uint64_t bindingGeneration{};
    std::uint8_t generation{};
    bool pending{};
    bool staged{};
};

/** Which inventory one world reward lands in. */
enum class WorldRewardKind : std::uint8_t {
    item,
    profileItem,
};

/** One reward earned in world, held until a Family-4 peer can publish it. */
struct WorldRewardRequest {
    std::uint64_t id{};
    std::int32_t quantity{};
    std::uint16_t itemDefinitionIndex{};
    WorldRewardKind kind{};
};
/** The changed character projections still owed after their source state commits. */
enum class CharacterRefreshScope : std::uint8_t { none, records, recordsAndRoster };

/** Mutable transport state owned by one BAP connection. */
struct Session {
    /** Actual BAP TCP source; never accepted from published native address bytes. */
    std::uint32_t remoteAddress{};
    nat_relay::State relay{};
    state::AccountHandle accountHandle{state::kInvalidAccount};
    std::uint64_t activityAdvertisementHostGeneration{};
    std::uint64_t acquisitionPresentationUntilTick{};
    std::array<encrypted::queuez::AcquisitionPresentationRow,
               encrypted::queuez::kAcquisitionPresentationRowCapacity>
        acquisitionPresentationRows{};
    std::uint32_t id{};
    std::uint32_t activityRosterGroups{};
    std::int32_t pendingSeasonalExperienceAmount{};
    std::uint32_t pendingSeasonalExperienceMutationSerial{};
    bool authenticated{};
    /** Owes one family-five snapshot for the unlock overrides an artifact change moved. */
    bool artifactRefreshArmed{};
    bool artifactFamily4RefreshArmed{};
    std::uint64_t artifactFamily4RefreshDueTick{};
    state::ArtifactResetResult artifactResetRefresh{};
    std::size_t artifactResetRefreshCursor{};
    std::uint8_t acquisitionPresentationRowCount{};
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::array<std::byte, state::kBapNonceSize> receiveNonce{};
    /** This connection's own AES-GCM key. Sharing one across links reuses key and nonce pairs. */
    std::array<std::byte, state::kAesKeySize> sessionKey{};
    /** Opaque State handle taken only after the server hello authenticates. */
    state::matchmaking::ContextHandle matchmakingContext{};
    /** Exact private or public ActivityClient generation owned by this connection. */
    ActivityClientBinding activity{};
    /** Tick count after which the activity link owes its next keepalive write. */
    std::uint64_t activityKeepaliveDueTick{};
    /** Client member key from the join request. It seeds the membership id. */
    std::uint64_t activityMemberKey{};
    /** Sparse native transport report owned by this exact activity binding. */
    middleware::bap::activity_message::TransportReport activityTransport{};
    encrypted::activity_host_manager::PendingStartupReservations activityStartupReservations{};
    /** Binding generation whose entity-slot join committed, including a zero member key. */
    std::uint64_t activityJoinGeneration{};
    /** Original native join correlation used for member-set rejoin notifications. */
    std::uint32_t activityJoinCorrelation{};
    std::array<std::uint64_t, state::activity::entity_slots::kMemberLeaseRowCount>
        activityMemberSet{};
    /**
     * Character the join request named, or zero when it carried none.
     * The roster's participation key must be the character the client signed in on. The client
     * binds its player by matching that value.
     */
    std::uint64_t activityCharacterSoid{};
    /**
     * Binding generation whose membership body this link has already delivered.
     * The client sets its membership flag once and never clears it, and never acknowledges a body
     * on a public-target link, so this is a one-shot per binding. Latched on delivery, not encode.
     */
    std::uint64_t activityMembershipSentGeneration{};
    /** Binding generation whose client-authored type-23 identity has committed. */
    std::uint64_t activityClientIdentitySeenGeneration{};
    /**
     * Binding generation whose committed type-23 identity reached the client in membership.
     * The initial roster must not consume its object-rebuild revisions before this latch.
     */
    std::uint64_t activityClientIdentityPublishedGeneration{};
    /**
     * Set when the join burst staged a membership body, and read once the frame is published.
     * The generation it belongs to does not exist until publication, so the latch cannot be written
     * at staging time.
     */
    bool activityJoinMembershipStaged{};
    /** The client's own patch epoch, scoped to the binding that received message 52. */
    BoundPatchEpoch activityPatchEpoch{};
    /** Replication epoch held until its encrypted frame reaches the transport caller. */
    ReplicationEpochPublication activityReplicationEpoch{};
    /** Per published group key: its registration identity and the revision the client holds. */
    std::array<RosterGroupLease, kRosterGroupLeaseCapacity> activityRosterGroupLeases{};
    /** Bubbles in the order their sub-block first went out; the client keys them by position. */
    std::array<std::uint8_t, state::build_data::scenarios::kBubbleCapacity>
        activityRosterBubbleOrder{};
    std::size_t activityRosterBubbleOrderCount{};
    /** Per bubble, its keys in the order they first went out; a later key only appends. */
    std::array<
        std::array<std::uint32_t,
                   middleware::bap::activity_message::sensor_auth_update::kBubbleKeyCapacity>,
        state::build_data::scenarios::kBubbleCapacity>
        activityRosterBubbleKeyOrder{};
    std::array<std::size_t, state::build_data::scenarios::kBubbleCapacity>
        activityRosterBubbleKeyOrderCount{};
    /**
     * A roster answer was refused for want of the client's patch epoch, and is still owed.
     * The epoch arriving is what makes that answer possible, so message 52 discharges it.
     */
    bool activityRosterOwedForEpoch{};
    /**
     * Binding generation whose leave delta reached the client, or zero.
     * The client reported it is leaving, so no later roster may go out on that link.
     */
    std::uint64_t activityLeftGeneration{};
    /**
     * Region-rebuild epoch, folded into every group's identity. It advances once per region
     * transition so every group's revision byte moves and the client re-registers the groups for
     * the region it entered. Without that move, object intents into the new bubble are refused.
     */
    std::uint8_t activityRosterRegionEpoch{};
    /** Bubble the epoch was stamped for; a change in it is the region transition. */
    std::int32_t activityRosterRegionBubble{-1};
    /** One-shot first-roster flag; the mission seed's adoption guard reads it. */
    std::uint8_t activityRosterSends{};
    /** Group-revision allocator; each value is spent on one group registration change. */
    std::uint8_t activityRosterState{};
    /** Latest Activity Host revision staged into this connection's transport output. */
    std::uint64_t activityHostStateRevision{};
    /** The delivered roster carries the team wait bit; the arrival report is answered while set. */
    bool activityRosterAwaitClientSync{};
    /** One bounded authority-mask readback owned by this exact ActivityClient generation. */
    authority_query::Owner activityAuthorityQuery{};
    /** One bounded authority-mask reset owned by this exact ActivityClient generation. */
    authority_reset::Owner activityAuthorityReset{};
    /** Operator incident held until the complete encrypted frame reaches the transport caller. */
    IncidentPublication activityIncidentStaged{};
    /** Earliest tick at which a refused incident frame may be rebuilt. */
    std::uint64_t activityIncidentRetryDueTick{};
    /** Host rows retained by the last delivered host directory. */
    AdvertisementRetains activityAdvertisementHeld{};
    /** Host rows retained by a staged membership body until publication is known. */
    AdvertisementPublication activityAdvertisementStaged{};
    /**
     * Reason code of the last logged roster outcome.
     * The push runs every second, so a refusal is logged only when the reason changes. One flag
     * for every reason hides the second failure behind the first.
     */
    std::uint8_t activityRosterReason{};
    /** What one staged roster body owes, and what to put back if it never reaches the caller. */
    RosterPublication activityRosterStaged{};
    /** Last complete msg-5 roster known to have reached this exact connection generation. */
    RosterDecodeMap activityRosterDecode{};
    /** Delivered squad Auth bodies, all re-emitted so phase-2 reset cannot clear any slot. */
    SquadOverrideLease activitySquadOverride{};
    /** Generated selected-state roster lease. Disabled until an operator enables its SDK row. */
    MissionSeedLease activityMissionSeed{};
    /** Queuez versions and residents published only through this authenticated peer. */
    encrypted::queuez::SessionState queuez{};
    encrypted::public_queuez::Subscriptions publicSubscriptions{};
    /** Tick count after which the owed Family-4 re-push may go out. */
    std::uint64_t family4RepushDueTick{};
    /** Root the owed re-push must use. */
    std::uint64_t family4RepushRoot{};
    /** True while one Family-4 re-push is still owed to this peer. */
    bool family4RepushArmed{};
    /** Tick count after which the owed banner re-push may go out. */
    std::uint64_t bannerRepushDueTick{};
    /** Root the owed banner re-push must use. */
    std::uint64_t bannerRepushRoot{};
    /** True while one banner re-push is still owed to this peer. */
    bool bannerRepushArmed{};
    /** Root the last family-two subscribe was answered against, and the re-push must reuse. */
    std::uint64_t socialRosterRepushRoot{};
    /** True while one family-two re-push is still owed to this peer. */
    bool socialRosterRepushArmed{};
    /** Social registration this link owns. Zero until a sync request registers it. */
    std::uint64_t socialSerial{};
    /** Publication stamp this link has already been told about, for feed and notice alike. */
    state::social::Stamp socialSentStamp{};
    /** Latest shared-account generation this peer has received. */
    std::uint64_t accountGeneration{};
    /** Newest shared-account generation owed as a full cross-peer refresh. */
    std::uint64_t accountResyncGeneration{};
    /** Set by encrypted processing only after one account mutation commits and is copied out. */
    bool accountMutationPublished{};
    /** True while another peer's account mutation still needs a full local refresh. */
    bool accountResyncArmed{};
    /**
     * Tick count after which the owed ability-icon refresh may go out. A subclass selection
     * invalidates the published ability buckets and the rebuild runs off the Client
     * content-extraction pump, so the inline refresh can carry empty ones; this one re-derives.
     */
    std::uint64_t abilityRefreshDueTick{};
    /** A roster change remains owed when later socket changes also refresh the character. */
    CharacterRefreshScope characterRefreshScope{};
    /**
     * True while the launch cinematic hold has parked the account player-record for this peer.
     * Set when the family-0 park push is armed on a loading launch, cleared when arrival arms the
     * family-4 completion that resolves it. Gates the two phases so each fires once per launch.
     */
    bool cinematicHeld{};
};

/** Guards the session table; hold it for the whole event and never re-enter it from a route. */
[[nodiscard]] core::threading::SrwLock& session_lock() noexcept;

/** @return Every session slot, open or not. Caller owns the session lock. */
[[nodiscard]] std::span<Session> sessions() noexcept;

/** @return True while any authenticated peer holds a Family-4 subscription. */
[[nodiscard]] bool has_active_family4_peer() noexcept;

/**
 * Finds one exact authenticated ActivityClient while the caller owns the session lock.
 * @param binding Exact Activity Host generation selected by the caller.
 * @param count Receives how many links own the binding.
 * @return The link when exactly one owns the binding, else null.
 */
[[nodiscard]] const Session*
unique_activity_link_locked(const state::activity::SessionBinding& binding,
                            std::size_t& count) noexcept;

/** @return Region index this connection's msg-5 builder selects. Caller owns the session lock. */
[[nodiscard]] std::int32_t selected_region_index_locked(const Session& session) noexcept;

/** Loads the scenario layout one lock-held ActivityClient owns. @return False when it has none. */
[[nodiscard]] bool
session_scenario_layout(const Session& session,
                        state::build_data::scenarios::Definition& output) noexcept;

/** Commits every queued world reward with no presentation and empties the queue. */
void drain_world_rewards() noexcept;

/** Arms every other Family-4 peer after the origin publishes a complete account mutation. */
void arm_account_resync_elsewhere(Session& origin) noexcept;

/** Arms every Family-4 peer, including the origin, for a full account resync. */
void arm_account_resync_everywhere() noexcept;

/** Holds this peer's full Family-4 refreshes until its acquisition flyout has finished. */
void arm_acquisition_presentation_hold(Session& session) noexcept;

/** Queues one character item for normal acquisition feedback. */
[[nodiscard]] bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept;

/** Queues one profile material for normal acquisition feedback. */
[[nodiscard]] bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                                      std::int32_t quantity) noexcept;

/** Reads the oldest queued world reward without removing it. */
[[nodiscard]] bool current_world_reward(WorldRewardRequest& request) noexcept;

/** Removes the world reward returned by current_world_reward. */
[[nodiscard]] bool complete_world_reward(std::uint64_t id) noexcept;

/** Commits the queued reward with no flyout once its presentation cannot be built. */
void settle_world_reward() noexcept;

/** Queues one transient XP item update so the native HUD presents a seasonal XP gain. */
[[nodiscard]] bool arm_seasonal_experience_presentation(std::int32_t amount) noexcept;

/**
 * Finds one unambiguous registry identity in a committed connection-local roster map.
 * @param map Last complete msg-5 map delivered on one BAP connection.
 * @param expectedBindingGeneration Exact ActivityClient generation routing the client message.
 * @param registryKey Client-reported registry key from that roster.
 * @return The unique entry, or null for an invalid/stale map, absent key, or duplicate key.
 */
[[nodiscard]] const RosterDecodeEntry*
find_roster_decode_entry(const RosterDecodeMap& map,
                         std::uint64_t expectedBindingGeneration,
                         std::uint32_t registryKey) noexcept;

namespace plaintext {

/**
 * Handles plaintext bootstrap services, arms encryption after service 25, and routes the rest.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Parsed outer frame carrying the service id and its body.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the service owes no reply, or its response is encoded.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

} // namespace plaintext

namespace encrypted {

/**
 * Authenticates and routes one encrypted post-bootstrap service frame.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when routing works, any response fits, State commits and the nonce is published.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Sends the owed Family-4 re-push once its delay has passed.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole Family-4 notification is published.
 */
[[nodiscard]] bool consume_deferred(Session& session,
                                    Scratch& scratch,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool& touchesScratch) noexcept;

} // namespace encrypted

} // namespace sunrise::server::bap
