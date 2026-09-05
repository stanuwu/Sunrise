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

/** Keeps one staged membership body until its frame outcome is known. */
void stage_membership_body_record(const Session& session, std::span<const std::byte> body) noexcept;

/** Promotes the staged membership body to the delivered record. */
void commit_membership_body_record(const Session& session) noexcept;

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

/** Returns false when region publicity cannot be resolved, without treating it as public. */
[[nodiscard]] bool region_publicity(const Session& session, std::int32_t region,
                                    bool& isPublic) noexcept;

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
 * Tests whether the client is in a live world: it holds the region it reported and no host
 * move is waiting for its arrival.
 * @param session Connection whose activity session the client reports on.
 * @param refresh Refresh being answered, or null.
 */
[[nodiscard]] bool client_in_world(const Session& session, const RefreshReport* refresh) noexcept;

/**
 * Tests whether the client's destination region is instantiated far enough for the native spawn.
 * Unlike `client_in_world`, this deliberately does not require the post-spawn world-state 8
 * write-back: requiring that signal to release the spawn creates a circular wait.
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

/** FNV-1a over one encoded body, so two log lines can say whether the bytes repeated. */
[[nodiscard]] inline std::uint64_t body_hash(std::span<const std::byte> body) noexcept {
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
