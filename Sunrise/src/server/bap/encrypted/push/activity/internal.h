#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../gameplay/gameplay_advertisement.h"
#include "../../../internal.h"
#include "activity_arrival.h"
#include "activity_roster_device_publication.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace message = middleware::bap::activity_message::sensor_auth_update;

/** @return True when current msg 1 selects this exact authored region. */
[[nodiscard]] constexpr bool
msg1_selects_region(const state::build_data::scenarios::Definition& layout,
                    std::int32_t region) noexcept {
    namespace tables = middleware::content::packages::tables;
    if (region < 0) {
        return false;
    }
    const auto selected = static_cast<std::uint32_t>(region);
    const std::uint32_t bubble = selected / tables::kSliceSetIndexFactor;
    const std::uint32_t selectedState = selected % tables::kSliceSetIndexFactor;
    return bubble < layout.bubbleCount && bubble < layout.bubbleStates.size()
           && layout.bubbleStates[bubble] == state::build_data::scenarios::kBubbleEnabledByte
           && selectedState < layout.bubbleStateCounts[bubble];
}

/**
 * Prepares the fallback membership identity when no identity message has arrived.
 * @param sessionId Joined activity session.
 * @param memberKey Client member key captured from the join request.
 * @param characterSoid Character the client signed in on.
 * @param mutation Cleared, then receives the exact deferred identity operation.
 * @return True when a membership snapshot can be staged.
 */
[[nodiscard]] bool
prepare_seed_identity(std::uint64_t sessionId,
                      std::uint64_t memberKey,
                      std::uint64_t characterSoid,
                      state::activity::membership::PendingMutation& mutation) noexcept;

/**
 * Builds the membership snapshot a first join commits, without reading State.
 * The join burst stages this body before the join commit; the commit then lands the same
 * seed identity, so the body and the record agree at the initial revision.
 * @param createdRevision Activity record generation used as the replacement-world epoch.
 * @param memberKey Client member key from the join request.
 * @param characterSoid Character the join request named, or zero.
 * @param mutation Cleared, then receives the snapshot alone; nothing here is committable.
 * @return True when the key is usable.
 */
[[nodiscard]] bool
prepare_join_seed_snapshot(std::uint64_t createdRevision,
                           std::uint64_t memberKey,
                           std::uint64_t characterSoid,
                           state::activity::membership::PendingMutation& mutation) noexcept;

/** Adopts the join burst's staged membership body under the connection's new generation. */
void adopt_join_membership_record(const Session& session) noexcept;

/** Why one roster push produced nothing, or that it produced a body. */
enum class RosterOutcome : std::uint8_t {
    published,
    noEpoch,
    noLayout,
    noGroups,
    noOverrideTarget,
    encodeFailed,
    unchanged,
};

/** @return True when this body equals the last roster body delivered on this connection. */
[[nodiscard]] bool repeats_delivered_roster_body(const Session& session,
                                                 std::span<const std::byte> body) noexcept;

/** Keeps one staged roster body until its frame outcome is known. */
void stage_roster_body_record(const Session& session, std::span<const std::byte> body) noexcept;

/** Promotes the staged roster body to the delivered record. */
void commit_roster_body_record(const Session& session) noexcept;

/** Drops the staged roster body of a discarded frame. */
void discard_roster_body_record(const Session& session) noexcept;

/** @return True when this body equals the last membership body delivered on this connection. */
[[nodiscard]] bool repeats_delivered_membership_body(const Session& session,
                                                     std::span<const std::byte> body) noexcept;

/**
 * Keeps one staged membership body until its frame outcome is known.
 * @param sessionId Activity session whose membership revision this body carries.
 * @param revision Membership revision encoded in the body.
 */
void stage_membership_body_record(const Session& session,
                                  std::span<const std::byte> body,
                                  std::uint64_t sessionId,
                                  std::uint32_t revision) noexcept;

/** Promotes the staged membership body to the delivered record. */
void commit_membership_body_record(const Session& session) noexcept;

/** Drops membership state that belonged to a discarded frame. */
void discard_membership_body_record(const Session& session) noexcept;

/** Names where this body first differs from the last one delivered on this connection. */
void report_roster_body_delta(const Session& session, std::span<const std::byte> body) noexcept;

/**
 * Reports one unsolicited push held back while the client holds no slice set, once per held item.
 * @param hostStateRevision Host state revision waiting, or zero.
 * @param scriptableRevision Typed body waiting, or zero.
 */
void report_roster_deferral(const Session& session,
                            std::uint64_t hostStateRevision,
                            std::uint64_t scriptableRevision) noexcept;

/**
 * Tests whether this connection has itself delivered one membership revision.
 * The client applies one membership update per revision and the acknowledgement is stored on the
 * member row, so a revision another link of the same member acknowledged would otherwise close the
 * publish trigger for a link that never carried it.
 * @param session Connection to test.
 * @param sessionId Activity session the revision belongs to.
 * @param revision Current membership revision of that session.
 * @return True when this connection still owes the revision.
 */
[[nodiscard]] bool connection_owes_membership(const Session& session,
                                              std::uint64_t sessionId,
                                              std::uint32_t revision) noexcept;

/**
 * Records this recipient's receipt for the membership body this connection delivered.
 * Multiple connections can share one member row, whose acknowledgement covers the session.
 * This receipt records the connection's own answer for its delivered body. It ignores a revision
 * this connection did not itself deliver on the current binding.
 * @param session Connection the acknowledgement arrived on.
 * @param revision Membership revision the client says it applied.
 */
void note_membership_acknowledgement(const Session& session, std::uint32_t revision) noexcept;

/**
 * Tests whether this connection's last delivered membership body has been acknowledged.
 * @param session Connection to test.
 * @return True when this recipient answered for the exact body this link delivered.
 */
[[nodiscard]] bool connection_membership_acknowledged(const Session& session) noexcept;

/**
 * Tests whether the installed packages author one region as private.
 * @param source Exact activity session the generated world is resolved for.
 * @param bindingGeneration Connection generation that binds it.
 * @param region Region index; a negative one is not private.
 */
[[nodiscard]] bool private_region(const state::activity::SessionBinding& source,
                                  std::uint64_t bindingGeneration,
                                  std::int32_t region) noexcept;

/** Same test for the connection's own activity session. */
[[nodiscard]] bool private_region(const Session& session, std::int32_t region) noexcept;

/** True only for an authored public region in this exact activity selection. */
[[nodiscard]] bool public_region(const state::activity::SessionBinding& source,
                                 std::uint64_t bindingGeneration,
                                 std::int32_t region) noexcept;

/**
 * Reads the authored publicity of every bubble from the installed packages.
 * @param session Exact ActivityClient owner whose generated world is read.
 * @param mask Receives one bit per public bubble.
 * @return False when no world is bound, in which case the mask is left clear.
 */
[[nodiscard]] bool region_publicity_mask(const Session& session, std::uint64_t& mask) noexcept;

/**
 * Reports whether the citizen advertisement for one region can be built now.
 * A private region has no citizen join and hosts its own bubbles. It advertises nothing and
 * claims no host row. Only a public region asks the gameplay host for one.
 * @param session Connection whose source binding the advertisement is built from.
 * @param region Region the body publishes.
 */
[[nodiscard]] server::gameplay::AdvertisementState
region_advertisement(const Session& session, std::int32_t region) noexcept;

/**
 * Selects the region field for one already-validated ActivityClient binding.
 * @param role Private-current or public-target connection role.
 * @param privateReportedRegion Latest private source report, or the absent sentinel.
 * @param publicAdvertisedRegion Immutable public host-binding region, or the absent sentinel.
 * @param arrival Destination arrival used only as the private pre-report fallback.
 * @return Exactly the region msg 5 uses for that role.
 */
[[nodiscard]] constexpr EffectiveRegion
select_activity_client_region(ActivityClientRole role,
                              std::int32_t privateReportedRegion,
                              std::int32_t publicAdvertisedRegion,
                              std::uint16_t arrival) noexcept {
    EffectiveRegion region{};
    region.index = state::activity::membership::kAbsentRegionIndex;
    region.arrival = arrival;
    if (role == ActivityClientRole::publicTarget) {
        region.index = publicAdvertisedRegion;
        region.reported = region.index >= 0;
    } else if (role == ActivityClientRole::privateCurrent) {
        region.reported = privateReportedRegion >= 0;
        region.index = region.reported ? privateReportedRegion : static_cast<std::int32_t>(arrival);
    }
    return region;
}

/** Client placement fields read from a mutation while its State commit is still pending. */
struct RefreshReport final {
    std::int32_t bubble{};
    std::uint32_t revision{};
    std::int32_t currentRegion{state::activity::membership::kAbsentRegionIndex};
    bool hasCurrentRegion{};
};

/**
 * Reads where the client says it is.
 * @param session Connection whose activity session the client reports on.
 * @param refresh Report being answered, which stands in for its uncommitted placement, or null.
 */
[[nodiscard]] state::activity::membership::ClientPlacement
client_placement(const Session& session, const RefreshReport* refresh) noexcept;

/**
 * Tests whether the client has reported arrival in its instantiated region: its ws-702 world
 * state reached 8 while it holds the region it reported and no host move is waiting. This is
 * the report that releases the native spawn gate.
 * @param session Connection whose activity session the client reports on.
 * @param refresh Refresh being answered, or null.
 */
[[nodiscard]] bool client_in_world(const Session& session, const RefreshReport* refresh) noexcept;

/**
 * Tests whether the client's destination region is instantiated, before its arrival report.
 * This advances the loading lifetime. The spawn gate itself waits for `client_in_world`.
 * @param session Connection whose activity session the client reports on.
 * @param refresh Refresh being answered, or null.
 */
[[nodiscard]] bool client_region_ready(const Session& session,
                                       const RefreshReport* refresh) noexcept;

/**
 * Resolves the exact region one selected BAP ActivityClient would put in msg 5.
 * @param session Lock-owned authenticated connection state.
 * @param arrival Destination arrival already resolved from the same layout as the roster.
 * @return The builder's private reported/fallback or public advertised region.
 */
[[nodiscard]] EffectiveRegion selected_effective_region(const Session& session,
                                                        std::uint16_t arrival) noexcept;

/**
 * Copies the decode identities from one complete, already-encoded msg-5 roster snapshot.
 *
 * @param roster Exact roster whose group order was written to the client.
 * @param bindingGeneration ActivityClient generation that owns the outbound frame.
 * @param output Cleared, then receives the validated fixed-capacity map.
 * @return True when the group count fits and every registry key is unique.
 */
[[nodiscard]] bool build_roster_decode_map(const message::Roster& roster,
                                           std::uint64_t bindingGeneration,
                                           RosterDecodeMap& output) noexcept;

/** Activates one staged squad lease only at transport commit after live-state revalidation. */
[[nodiscard]] bool activate_staged_squad_override(Session& session) noexcept;

/** The player key this link's message 5 binds: the join character's SOID, or its identity. */
[[nodiscard]] std::uint64_t published_player_key(const Session& session) noexcept;

/** Restores the roster counters for a discarded publication without changing its retained lease. */
void rollback_staged_roster_state(Session& session) noexcept;

/** One committed body riding out on the same push as the head. */
struct TailAuthOverride final {
    middleware::bap::activity_message::sensor_auth_update::AuthOverride value{};
    std::uint16_t rosterGroupIndex{};
    std::uint16_t rosterSlotOffset{};
    bool stateLocalRosterTarget{};
};

/**
 * Builds the roster body input for one session's current destination.
 * Each group carries its own revision from its lease, moved only when that group's identity
 * changes, so an unrelated change never rebuilds a group's objects.
 * @param session Connection-owned epoch, leases and counters, advanced on success.
 * @param scratch Lock-owned roster group storage the body's spans point into.
 * @param snapshot Cleared, then receives the epoch, roster and scalars.
 * @param destination Receives the destination name the push found.
 * @param destinationLength Receives its length.
 * @param epoch Epoch to echo for a body staged before its connection field is published.
 * @param lifetimeState Type-17 lifetime value copied into every roster state block.
 * @param authOverride Exact typed body for one published slot, or null.
 * @param rosterGroupIndex Canonical table row, or the generated-group sentinel.
 * @param rosterSlotOffset Selected slot's compressed offset inside that group.
 * @param stateLocalRosterTarget True only for the request-owned selected-state group.
 * @param stateLocalRegion Exact authored region that owns a state-local group.
 * @param sdkObjectIndex Generated object row, or the absent sentinel for a canonical group.
 * @param stateLocalRosterGroup Request-owned complete group, or null for a canonical group.
 * @param exactRegion Prepared transaction region, or null to resolve committed membership.
 * @param refresh The client refresh this body answers, or null.
 * @return published when the destination gives a roster that binds the player, or the refusal.
 */
[[nodiscard]] RosterOutcome build_roster_snapshot(
    Session& session,
    Scratch& scratch,
    message::Snapshot& snapshot,
    std::span<char> destination,
    std::size_t& destinationLength,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch* epoch = nullptr,
    std::uint8_t lifetimeState = 3,
    const message::AuthOverride* authOverride = nullptr,
    std::uint16_t rosterGroupIndex = 0,
    std::uint16_t rosterSlotOffset = 0,
    bool stateLocalRosterTarget = false,
    std::int32_t stateLocalRegion = -1,
    std::uint32_t sdkObjectIndex = 0xFFFFFFFFU,
    const state::build_data::scenarios::RosterGroup* stateLocalRosterGroup = nullptr,
    const EffectiveRegion* exactRegion = nullptr,
    const RefreshReport* refresh = nullptr,
    std::span<const TailAuthOverride> tailOverrides = {}) noexcept;

/** Result of joining a generated group against every group already staged in this roster. */
enum class ExistingGroup : std::uint8_t {
    missing,
    exact,
    conflict,
};

/** Whether one canonical roster row is registered in the bubble the body publishes. */
enum class CanonicalGroupStatus : std::uint8_t {
    unknown,
    inactive,
    active,
};

/** Logs which exit refused, since the returned outcome itself carries no reason. */
[[nodiscard]] RosterOutcome refuse_override(std::string_view reason) noexcept;

/** @return Authored SOID of the character the join named, or of the selected character. */
[[nodiscard]] std::uint64_t roster_player_key(std::uint64_t joinCharacter) noexcept;

/** @return True when two request-owned generated groups carry the same wire fields. */
[[nodiscard]] bool
same_generated_group(const state::build_data::scenarios::RosterGroup& left,
                     const state::build_data::scenarios::RosterGroup& right) noexcept;

/** Copies one request-owned generated group into the encoder's fixed input at one slot. */
[[nodiscard]] bool fill_generated_group(const state::build_data::scenarios::RosterGroup& source,
                                        Scratch& scratch,
                                        std::size_t slot,
                                        message::Roster& roster) noexcept;

/** Finds one exact same-key group and rejects conflicting or multiply-published keys. */
[[nodiscard]] ExistingGroup
find_existing_group(const state::build_data::scenarios::RosterGroup& candidate,
                    const Scratch& scratch,
                    const message::Roster& roster,
                    std::size_t& position) noexcept;

/** Ensures a reused non-top-level group is active in the exact requested bubble. */
[[nodiscard]] bool activate_existing_group(std::size_t position,
                                           std::uint32_t bubble,
                                           Scratch& scratch,
                                           message::Roster& roster) noexcept;

/** @return One bit per bubble this link hosts; every bubble when no world is bound. */
[[nodiscard]] std::uint64_t hosted_bubble_mask(const Session& session) noexcept;

/** @return True when this link registers the destination's top-level groups. */
[[nodiscard]] bool publishes_top_level_groups(const Session& session) noexcept;

/** Copies the destination's published groups into the encoder's fixed input. */
[[nodiscard]] bool fill_roster(const state::build_data::scenarios::Definition& layout,
                               std::uint64_t hostedBubbles,
                               bool publishTopLevel,
                               Scratch& scratch,
                               message::Roster& roster,
                               bool includeTopLevel = true) noexcept;

/** Appends one selected-state group and registers its key in its exact authored bubble. */
[[nodiscard]] bool
append_state_local_group(const state::build_data::scenarios::RosterGroup& generatedGroup,
                         std::uint32_t bubble,
                         Scratch& scratch,
                         message::Roster& roster) noexcept;

/** @return True when one retained entry names this exact slot inside its shared group. */
[[nodiscard]] bool
same_retained_target(const RetainedSquadGroup& group,
                     const RetainedSquadAuth& retained,
                     const server::activity::host::ScriptableTarget& target) noexcept;

/** @return Dense retained-group index for this target, or groupCount when it is new. */
[[nodiscard]] std::size_t
retained_group_index(const SquadOverrideLease& lease,
                     const server::activity::host::ScriptableTarget& target) noexcept;

/** Copies one already-encoded pending body into the compact retained representation. */
[[nodiscard]] bool
make_retained_squad_auth(const server::activity::host::PendingScriptableOverride& pending,
                         std::uint64_t bindingGeneration,
                         RetainedSquadAuth& output) noexcept;

/** Restores one committed squad body exactly so phase-2 reset cannot clear its slot. */
[[nodiscard]] bool retained_squad_auth(const SquadOverrideLease& lease,
                                       std::size_t index,
                                       message::AuthOverride& output) noexcept;

/** @return True when every retained group and body is safe for cumulative publication. */
[[nodiscard]] bool valid_retained_squad_lease(const SquadOverrideLease& lease,
                                              std::uint64_t bindingGeneration) noexcept;

/** @return Whether one canonical group is registered in the selected bubble. */
[[nodiscard]] CanonicalGroupStatus
canonical_group_status(const state::build_data::scenarios::Definition& layout,
                       const EffectiveRegion& region,
                       std::uint16_t tableIndex) noexcept;

/** Installs an override only when its canonical roster row is registered in this exact bubble. */
[[nodiscard]] bool install_auth_override(const state::build_data::scenarios::Definition& layout,
                                         const EffectiveRegion& region,
                                         Scratch& scratch,
                                         message::Snapshot& snapshot,
                                         const message::AuthOverride& value,
                                         std::uint16_t tableIndex,
                                         std::uint16_t slotOffset,
                                         bool stateLocalRosterTarget) noexcept;

/** Copies one delivered Host body into the message codec's value type. */
[[nodiscard]] bool
make_auth_override(const server::activity::host::PendingScriptableOverride& retained,
                   message::AuthOverride& output) noexcept;

/** Tracks the bubble the client holds; a change moves no group and only logs the crossing. */
void advance_region_epoch(Session& session, const RefreshReport* refresh) noexcept;

/**
 * Puts the groups appended after the seed into first-seen order, whichever path appended them.
 * The client keys its state bytes by position, so a group that moves is torn down and rebuilt.
 * @param firstAppended Position of the first group after the seed; earlier ones keep their order.
 */
void order_appended_groups(const Session& session,
                           Scratch& scratch,
                           message::Roster& roster,
                           std::size_t firstAppended) noexcept;

/**
 * Moves the groups of objects present in every scenario state into the top-level list.
 * The owning link also adds any such group not yet published; the other link retires them.
 * @param firstAppended Position of the first group after the seed; moved up for each group
 * taken from past it.
 * @return False when the scenario cannot be read or a group conflicts.
 */
[[nodiscard]] bool promote_scenario_wide_groups(const Session& session,
                                                Scratch& scratch,
                                                message::Roster& roster,
                                                std::size_t& firstAppended) noexcept;

/**
 * Registers every leased key under each bubble it was registered under before.
 * @return False when a sub-block is full.
 */
[[nodiscard]] bool
retain_group_bubbles(const Session& session, Scratch& scratch, message::Roster& roster) noexcept;

/**
 * Puts the bubble sub-blocks into first-seen order and records any bubble seen for the first time.
 * The client keys sub-blocks by position, so a moved block rebuilds every group it lists.
 */
void order_sub_blocks(Session& session, Scratch& scratch, message::Roster& roster) noexcept;

/** Stamps every group's revision from its lease, moved only when that group's identity changes. */
void stamp_group_sequences(Session& session, message::Roster& roster) noexcept;

/** FNV-1a over one encoded body, so two log lines can say whether the bytes repeated. */
[[nodiscard]] inline std::uint64_t body_hash(std::span<const std::byte> body) noexcept {
    // FNV-1a 64-bit offset basis and prime.
    constexpr std::uint64_t kBasis = 0xCBF29CE484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001B3ULL;
    std::uint64_t hash = kBasis;
    for (const std::byte value : body) {
        hash ^= std::to_integer<std::uint64_t>(value);
        hash *= kPrime;
    }
    return hash;
}

/** Terms that stop an unsolicited roster body being skipped as a repeat. Logged as `force=`. */
inline constexpr std::uint8_t kRosterForceSolicited = 0x01;
inline constexpr std::uint8_t kRosterForceGrant = 0x02;
inline constexpr std::uint8_t kRosterForceHostState = 0x04;
inline constexpr std::uint8_t kRosterForceScriptable = 0x08;
inline constexpr std::uint8_t kRosterForceMissionSeed = 0x10;

/**
 * Reports one roster push, and only when its outcome is new.
 * @param session Connection-owned roster counters, whose last reported reason is updated.
 * @param snapshot Body input, carrying the region, arrival and spawn set.
 * @param destination Destination name the push found; may be empty.
 * @param bytes Encoded body size, or zero when nothing was staged.
 * @param grant Bubble granted with this body, or -1.
 * @param outcome What the push produced.
 * @param bodyHash FNV-1a of the encoded body, or zero when nothing was encoded.
 * @param forced The `kRosterForce*` terms that held for this body.
 */
void report_roster_push(Session& session,
                        const message::Snapshot& snapshot,
                        std::string_view destination,
                        std::size_t bytes,
                        std::int32_t grant,
                        RosterOutcome outcome,
                        std::uint64_t bodyHash,
                        std::uint8_t forced) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
