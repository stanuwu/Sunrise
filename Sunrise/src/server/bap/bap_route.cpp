#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/activity/runtime.h"
#include "../../state/build_data/runtime.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "../../state/runtime/runtime.h"
#include "../activity/host_runtime.h"
#include "activity_authority_query_owner.h"
#include "activity_authority_reset_owner.h"
#include "activity_mission_seed_lease.h"
#include "core/threading/srw_lock.h"
#include "encrypted/bap_connection_publication.h"
#include "encrypted/push/activity/mission_seed_world_change.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {

namespace layouts = state::build_data::scenarios;
namespace tables = middleware::content::packages::tables;

core::threading::SrwLock g_lock{};
std::array<Session, kSessionCount> g_sessions{};
Scratch g_scratch{};

/** Client-owned investment effects. Empty until the Client registers, and after it shuts down. */
std::atomic<InvestmentPublicationConsumer> g_investmentPublicationConsumer{};
std::atomic<InvestmentSliceConsumer> g_investmentSliceConsumer{};

/** Lifetime of the native item-acquisition flyout, which the hold must outlast. */
constexpr std::uint64_t kAcquisitionPresentationHoldMs = 8'000;

/** Arms every other active peer after one shared-account transaction is published. */
void publish_account_mutation(Session& origin) noexcept {
    origin.accountMutationPublished = false;
    arm_account_resync_elsewhere(origin);
}

/** @param id Nonzero connection id. @return Matching open session, or null. */
[[nodiscard]] Session* session_for(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return nullptr;
    }
    auto& session = g_sessions[id - 1];
    return session.id == id ? &session : nullptr;
}

/** @return True when every scalar and captured destination field is identical. */
[[nodiscard]] bool
same_destination(const state::activity::destination::DestinationSelection& left,
                 const state::activity::destination::DestinationSelection& right) noexcept {
    return left.packageName == right.packageName
           && left.packageNameLength == right.packageNameLength && left.reason == right.reason
           && left.sourceActivityIndex == right.sourceActivityIndex
           && left.activityIndex == right.activityIndex && left.elementIndex == right.elementIndex
           && left.selectionNonce == right.selectionNonce
           && left.arrivalBubbleHash == right.arrivalBubbleHash
           && left.spawnSetHash == right.spawnSetHash
           && left.hasElementIndex == right.hasElementIndex
           && left.hasSelectionNonce == right.hasSelectionNonce
           && left.hasArrivalBubbleHash == right.hasArrivalBubbleHash
           && left.hasSpawnSetHash == right.hasSpawnSetHash
           && left.arrivalBubbleOverride == right.arrivalBubbleOverride
           && left.hasArrivalBubbleOverride == right.hasArrivalBubbleOverride
           && left.sliceSetOverride == right.sliceSetOverride
           && left.hasSliceSetOverride == right.hasSliceSetOverride
           && left.spawnSetOverride == right.spawnSetOverride
           && left.hasSpawnSetOverride == right.hasSpawnSetOverride
           && left.descriptorBits == right.descriptorBits
           && left.descriptorBitLength == right.descriptorBitLength
           && left.descriptorNameBit == right.descriptorNameBit
           && left.hasDescriptorName == right.hasDescriptorName;
}

/** Returns the mutable unique-link result while the caller owns the exclusive BAP lock. */
[[nodiscard]] Session*
unique_mutable_activity_link_locked(const state::activity::SessionBinding& binding,
                                    std::size_t& count) noexcept {
    return const_cast<Session*>(unique_activity_link_locked(binding, count));
}

/** Validates and, when stale, clears one connection-owned SDK selected-state roster lease. */
[[nodiscard]] ActivityMissionSeedLeaseStatus
mission_seed_link_locked(const state::activity::SessionBinding& binding,
                         std::uint32_t scenarioRow,
                         std::uint64_t expectedGeneration,
                         Session*& output,
                         std::size_t& matchingLinks) noexcept {
    output = unique_mutable_activity_link_locked(binding, matchingLinks);
    if (output == nullptr) {
        return ActivityMissionSeedLeaseStatus::noActivityLink;
    }
    return mission_seed_session_status(*output, scenarioRow, expectedGeneration);
}

/** @return True when two leases name the same complete generated plan. */
[[nodiscard]] bool same_mission_seed_plan(const ActivityMissionSeedPlan& left,
                                          const ActivityMissionSeedPlan& right) noexcept {
    if (left.omissionCount != right.omissionCount) {
        return false;
    }
    for (std::uint32_t index = 0; index < left.omissionCount; ++index) {
        if (left.omissions[index].objectTag != right.omissions[index].objectTag
            || left.omissions[index].registryKey != right.omissions[index].registryKey) {
            return false;
        }
    }
    return left.activityRow == right.activityRow && left.scenarioRow == right.scenarioRow
           && left.stateRow == right.stateRow && left.bubbleRow == right.bubbleRow
           && left.bubbleOrdinal == right.bubbleOrdinal && left.stateOrdinal == right.stateOrdinal
           && left.entryIndex == right.entryIndex && left.sliceSetIndex == right.sliceSetIndex
           && left.effectiveRegion == right.effectiveRegion
           && left.occurrenceCount == right.occurrenceCount && left.groupCount == right.groupCount
           && left.authMappingSlots == right.authMappingSlots
           && left.authResetSlots == right.authResetSlots
           && left.senseSuppressedSlots == right.senseSuppressedSlots;
}

/** Validates only identities required to mutate a connection-owned lease safely. */
[[nodiscard]] bool valid_mission_seed_plan(const ActivityMissionSeedPlan& plan,
                                           std::uint32_t scenarioRow) noexcept {
    const std::uint64_t authoredRegion =
        static_cast<std::uint64_t>(plan.sliceSetIndex) + plan.stateOrdinal;
    return plan.activityRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.scenarioRow == scenarioRow
           && plan.stateRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.bubbleRow != (std::numeric_limits<std::uint32_t>::max)()
           && plan.bubbleOrdinal < layouts::kBubbleCapacity
           && plan.stateOrdinal < tables::kSliceSetIndexFactor
           && authoredRegion == plan.effectiveRegion
           && authoredRegion
                  <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
}

/** @param session Its secrets and identity are wiped. */
void clear_session(Session& session) noexcept {
    SecureZeroMemory(&session, sizeof session);
    // Zeroing is not the cleared state: `advertisedRegion` is -1 and zero is a real region.
    session.activity = {};
}

/**
 * Releases an authenticated session's optional matchmaking context.
 * @param session Open session that may not have finished server hello.
 * @return True when there was no context, or the active generation was released.
 */
[[nodiscard]] bool release_matchmaking_context(Session& session) noexcept {
    if (session.matchmakingContext.generation == state::matchmaking::kInvalidGeneration) {
        session.matchmakingContext = {};
        return true;
    }
    if (!state::matchmaking::release_context(session.matchmakingContext)) {
        return false;
    }
    session.matchmakingContext = {};
    return true;
}

/** @param id Session-slot id. @return True when the slot is opened. */
[[nodiscard]] bool open_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    session.id = id;
    return true;
}

/** @param id Session-slot id. @return True when the slot is cleared. */
[[nodiscard]] bool close_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    return true;
}

/**
 * Routes one validated frame through its connection-owned session.
 * @param request Frame event and caller-owned buffers.
 * @param response Receives encoded response size.
 * @return True when the frame is valid and its service is handled.
 */
[[nodiscard]] bool consume_frame(const client::network::BapRequest& request,
                                 client::network::BapResponse& response) noexcept {
    middleware::bap::OuterFrame frame;
    if (!middleware::bap::parse_frame(request.frame, frame)) {
        return false;
    }
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    bool handled = false;
    if (frame.frameType == middleware::bap::FrameType::encrypted) {
        handled = encrypted::consume(*session, g_scratch, frame, request.response, response.size);
    } else {
        handled = plaintext::consume(*session, g_scratch, frame, request.response, response.size);
    }
    if (!handled) {
        return false;
    }
    if (frame.frameType == middleware::bap::FrameType::encrypted
        && session->accountMutationPublished) {
        publish_account_mutation(*session);
    }
    // A frame response can carry one already-due push in the same bounded socket write.
    bool touchesScratch = true;
    std::size_t deferred = 0;
    if (response.size < request.response.size()
        && encrypted::consume_deferred(*session,
                                       g_scratch,
                                       request.response.subspan(response.size),
                                       deferred,
                                       touchesScratch)) {
        response.size += deferred;
    }
    return true;
}

/**
 * Services one timed poll for a session that may owe a deferred push.
 * @param request Poll event and caller-owned output buffer.
 * @param response Receives encoded notification size.
 * @param touchesScratch Set when the attempt reaches a scratch buffer.
 * @return True when a notification is published.
 */
[[nodiscard]] bool consume_poll(const client::network::BapRequest& request,
                                client::network::BapResponse& response,
                                bool& touchesScratch) noexcept {
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    return encrypted::consume_deferred(
        *session, g_scratch, request.response, response.size, touchesScratch);
}

} // namespace

/** Guards the session table; every locked helper below needs it held. */
core::threading::SrwLock& session_lock() noexcept {
    return g_lock;
}

/** @return Every session slot, open or not. */
std::span<Session> sessions() noexcept {
    return g_sessions;
}

/** Takes both consumer slots, or neither. */
bool register_client_investment_consumers(InvestmentPublicationConsumer publication,
                                          InvestmentSliceConsumer slice) noexcept {
    if (publication == nullptr || slice == nullptr
        || g_investmentPublicationConsumer.load(std::memory_order_acquire) != nullptr) {
        return false;
    }
    g_investmentSliceConsumer.store(slice, std::memory_order_release);
    // Published last, so it is the one slot that says the set is complete.
    g_investmentPublicationConsumer.store(publication, std::memory_order_release);
    return true;
}

/** Releases both consumer slots. */
void unregister_client_investment_consumers() noexcept {
    g_investmentPublicationConsumer.store(nullptr, std::memory_order_release);
    g_investmentSliceConsumer.store(nullptr, std::memory_order_release);
}

void notify_investment_publication() noexcept {
    const auto consumer = g_investmentPublicationConsumer.load(std::memory_order_acquire);
    if (consumer != nullptr) {
        consumer();
    }
}

void request_investment_slice() noexcept {
    const auto consumer = g_investmentSliceConsumer.load(std::memory_order_acquire);
    if (consumer != nullptr) {
        consumer();
    }
}

/** @return True while any authenticated peer holds a Family-4 subscription. */
bool has_active_family4_peer() noexcept {
    return std::any_of(g_sessions.begin(), g_sessions.end(), [](const Session& session) {
        return session.id != 0 && session.authenticated && session.queuez.family4Active;
    });
}

/** Finds one exact authenticated ActivityClient while the caller owns the BAP lock. */
const Session* unique_activity_link_locked(const state::activity::SessionBinding& binding,
                                           std::size_t& count) noexcept {
    count = 0;
    const Session* selected = nullptr;
    for (const Session& session : g_sessions) {
        const auto& owned = session.activity.session;
        if (session.id == 0 || !session.authenticated
            || session.activity.role == ActivityClientRole::none
            || session.activity.bindingGeneration == 0 || owned.sessionId != binding.sessionId
            || owned.createdRevision != binding.createdRevision
            || !same_destination(owned.destination, binding.destination)) {
            continue;
        }
        selected = &session;
        ++count;
    }
    return count == 1 ? selected : nullptr;
}

/** Loads the exact scenario layout owned by one lock-held ActivityClient. */
bool session_scenario_layout(const Session& session, layouts::Definition& output) noexcept {
    output = {};
    const auto& selection = session.activity.session.destination;
    if (!state::activity::binding_matches(session.activity.session)
        || selection.packageNameLength > selection.packageName.size()) {
        return false;
    }
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    return state::build_data::find_scenario_layout(name, output);
}

void arm_account_resync_elsewhere(Session& origin) noexcept {
    for (auto& peer : g_sessions) {
        if (&peer != &origin && peer.id != 0 && peer.authenticated && peer.queuez.family4Active) {
            peer.accountResyncArmed = true;
        }
    }
}

/** Arms every active peer to re-read the account, including the origin. */
void arm_account_resync_everywhere() noexcept {
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active) {
            continue;
        }
        peer.accountResyncArmed = true;
    }
}

/** Extends this peer's flyout hold, clearing a lapsed overlay first. */
void arm_acquisition_presentation_hold(Session& session) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (now >= session.acquisitionPresentationUntilTick) {
        session.acquisitionPresentationRows = {};
        session.acquisitionPresentationRowCount = 0;
    }
    session.acquisitionPresentationUntilTick =
        (std::max)(session.acquisitionPresentationUntilTick, now + kAcquisitionPresentationHoldMs);
}

/**
 * Grants seasonal XP and queues its HUD notification on the first peer that can show it.
 * @param amount Positive XP to grant.
 * @return True when the XP was granted.
 */
bool arm_seasonal_experience_presentation(std::int32_t amount) noexcept {
    if (amount <= 0 || !state::grant_seasonal_experience(amount)) {
        return false;
    }
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active
            || peer.pendingSeasonalExperienceAmount
                   > (std::numeric_limits<std::int32_t>::max)() - amount) {
            continue;
        }
        peer.pendingSeasonalExperienceAmount += amount;
        return true;
    }
    // No peer can animate the gain, so the account image carries the new total instead.
    arm_account_resync_everywhere();
    return true;
}

/** Finds one unambiguous registry identity in a committed connection-local roster map. */
const RosterDecodeEntry* find_roster_decode_entry(const RosterDecodeMap& map,
                                                  std::uint64_t expectedBindingGeneration,
                                                  std::uint32_t registryKey) noexcept {
    if (!map.valid || expectedBindingGeneration == 0
        || map.bindingGeneration != expectedBindingGeneration || map.count > map.entries.size()) {
        return nullptr;
    }

    const RosterDecodeEntry* match = nullptr;
    for (std::size_t index = 0; index < map.count; ++index) {
        const RosterDecodeEntry& entry = map.entries[index];
        if (entry.registryKey != registryKey) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &entry;
    }
    return match;
}

/** Applies one serialized BAP connection lifecycle event. */
bool consume(const client::network::BapRequest& request,
             client::network::BapResponse& response) noexcept {
    response = {};
    const std::lock_guard lock(g_lock);
    bool success = false;
    // Polls report whether they reached scratch.
    bool touchesScratch = request.event != client::network::BapEvent::poll;
    // Hold the session lock across cryptographic counter reads and updates.
    switch (request.event) {
    case client::network::BapEvent::open:
        success = open_session(request.connectionId);
        break;
    case client::network::BapEvent::frame:
        success = consume_frame(request, response);
        break;
    case client::network::BapEvent::poll:
        success = consume_poll(request, response, touchesScratch);
        break;
    case client::network::BapEvent::close:
        success = close_session(request.connectionId);
        break;
    }
    // Decrypted frames can contain runtime-only keys or tokens, so scratch never outlives the call.
    if (touchesScratch) {
        SecureZeroMemory(&g_scratch, sizeof g_scratch);
    }
    return success;
}

/** Checks whether one ActivityClient can accept state-local output for its published roster. */
ActivityMissionSeedLeaseStatus
activity_mission_seed_available(const state::activity::SessionBinding& binding,
                                std::uint32_t scenarioRow,
                                std::uint64_t expectedGeneration) noexcept {
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    std::size_t matchingLinks = 0;
    ActivityMissionSeedLeaseStatus status =
        mission_seed_link_locked(binding, scenarioRow, expectedGeneration, session, matchingLinks);
    if (status == ActivityMissionSeedLeaseStatus::ready && session->activityRosterStaged.staged) {
        status = ActivityMissionSeedLeaseStatus::outputBusy;
    }
    return status;
}

/** Reads one exact ActivityClient's connection-scoped SDK selected-state roster lease. */
ActivityMissionSeedLeaseStatus
activity_mission_seed_lease(const state::activity::SessionBinding& binding,
                            std::uint32_t scenarioRow,
                            std::uint64_t expectedGeneration,
                            ActivityMissionSeedLeaseView& output) noexcept {
    output = {};
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    ActivityMissionSeedLeaseStatus status = mission_seed_link_locked(
        binding, scenarioRow, expectedGeneration, session, output.matchingLinks);
    if (session != nullptr) {
        output.activityClientGeneration = session->activity.bindingGeneration;
    }
    if (status == ActivityMissionSeedLeaseStatus::ready) {
        read_mission_seed_lease(*session, output.matchingLinks, output);
    }
    return status;
}

/** Selects one exact materialized state and advances its publication revision once. */
ActivityMissionSeedLeaseStatus
select_activity_mission_seed(const state::activity::SessionBinding& binding,
                             const ActivityMissionSeedPlan& plan,
                             std::uint64_t expectedGeneration) noexcept {
    const std::lock_guard lock(g_lock);
    Session* session = nullptr;
    std::size_t matchingLinks = 0;
    ActivityMissionSeedLeaseStatus status = mission_seed_link_locked(
        binding, plan.scenarioRow, expectedGeneration, session, matchingLinks);
    if (status == ActivityMissionSeedLeaseStatus::ready
        && !valid_mission_seed_plan(plan, plan.scenarioRow)) {
        status = ActivityMissionSeedLeaseStatus::refused;
    }
    if (status == ActivityMissionSeedLeaseStatus::ready) {
        MissionSeedLease& lease = session->activityMissionSeed;
        // Ordinary traversal can reach a plan's region before the script selects it, so an open
        // arrival window closes here when the client already holds that region.
        const auto placement = state::activity::membership::reported_placement(binding.sessionId);
        const auto heldRegion = state::activity::membership::instantiated_region(placement);
        const bool targetHeld = encrypted::push::activity::mission_seed_arrival_window_closed(
            heldRegion, plan.effectiveRegion);
        if (lease.configured && same_mission_seed_plan(lease.plan, plan)) {
            if (targetHeld) {
                lease.regionArrivalPending = false;
            }
            // The script may select the plan the roster adopted by default. That is a selection.
            lease.scriptSelected = true;
            return ActivityMissionSeedLeaseStatus::ready;
        }
        if (lease.configured && lease.revision == (std::numeric_limits<std::uint64_t>::max)()) {
            status = ActivityMissionSeedLeaseStatus::refused;
        } else {
            // A staged older revision may finish. Its commit cannot publish this newer revision.
            const std::uint64_t revision = lease.configured ? lease.revision + 1U : 1U;
            // Every region this lease has selected stays registered on the peer, so record the
            // new one and keep the earlier ones. Publication carries the union; dropping a group
            // does not unregister it, it only stops seeding records the peer still holds.
            if (!lease.configured) {
                lease.registeredRegionCount = 0;
            }
            bool regionKnown = false;
            for (std::size_t index = 0; index < lease.registeredRegionCount; ++index) {
                regionKnown = regionKnown || lease.registeredRegions[index] == plan.effectiveRegion;
            }
            if (!regionKnown) {
                if (lease.registeredRegionCount >= lease.registeredRegions.size()) {
                    return ActivityMissionSeedLeaseStatus::refused;
                }
                lease.registeredRegions[lease.registeredRegionCount++] = plan.effectiveRegion;
            }
            // A selection that replaces the world waits for the client's arrival there. One that
            // does not must close any window an earlier selection left open, because an open
            // window blocks publication and nothing else clears it.
            if (lease.configured
                && encrypted::push::activity::mission_seed_selection_needs_arrival(
                    lease.plan.effectiveRegion, plan.effectiveRegion, heldRegion)) {
                lease.previousPlan = lease.plan;
                lease.regionArrivalPending = true;
            } else {
                lease.regionArrivalPending = false;
            }
            lease.plan = plan;
            lease.bindingGeneration = session->activity.bindingGeneration;
            lease.revision = revision;
            lease.configured = true;
            lease.scriptSelected = true;
        }
    }
    return status;
}

/** Queues one generation-bound replication epoch request. */
bool request_replication_epoch(const state::activity::SessionBinding& binding,
                               std::uint64_t expectedGeneration,
                               std::uint8_t requestedEpoch) noexcept {
    const std::lock_guard lock(g_lock);
    std::size_t count = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, count);
    bool queued =
        session != nullptr && expectedGeneration != 0
        && session->activity.bindingGeneration == expectedGeneration
        && requestedEpoch == static_cast<std::uint8_t>(session->activity.replicationEpoch + 1U);
    if (queued) {
        ReplicationEpochPublication& request = session->activityReplicationEpoch;
        queued = !request.pending
                 || (request.bindingGeneration == expectedGeneration
                     && request.generation == requestedEpoch);
        if (queued) {
            request.bindingGeneration = expectedGeneration;
            request.generation = requestedEpoch;
            request.pending = true;
            request.staged = false;
            session->activityKeepaliveDueTick = 0;
        }
    }
    return queued;
}

/** Queues one msg-30 readback on an exact unique ActivityClient link. */
ActivityAuthorityQueryStatus
request_activity_authority_query(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept {
    correlation = -1;
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, linkCount);
    ActivityAuthorityQueryStatus status = ActivityAuthorityQueryStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_query::request(
                           session->activityAuthorityQuery, expectedGeneration, correlation)
                     : ActivityAuthorityQueryStatus::staleActivityClient;
    }
    return status;
}

/** Copies one complete connection-owned authority readback. */
ActivityAuthorityQueryStatus
activity_authority_query_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityQuerySnapshot& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    ActivityAuthorityQueryStatus status = ActivityAuthorityQueryStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_query::snapshot(
                           session->activityAuthorityQuery, expectedGeneration, output)
                     : ActivityAuthorityQueryStatus::staleActivityClient;
    }
    return status;
}

/** Queues one msg-28 rebuild on an exact unique ActivityClient link. */
ActivityAuthorityResetStatus
request_activity_authority_reset(const state::activity::SessionBinding& binding,
                                 std::uint64_t expectedGeneration,
                                 std::int32_t& correlation) noexcept {
    correlation = -1;
    const std::lock_guard lock(g_lock);
    std::size_t linkCount = 0;
    Session* const session = unique_mutable_activity_link_locked(binding, linkCount);
    ActivityAuthorityResetStatus status = ActivityAuthorityResetStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_reset::request(
                           session->activityAuthorityReset, expectedGeneration, correlation)
                     : ActivityAuthorityResetStatus::staleActivityClient;
    }
    return status;
}

/** Copies one complete connection-owned authority-reset result. */
ActivityAuthorityResetStatus
activity_authority_reset_snapshot(const state::activity::SessionBinding& binding,
                                  std::uint64_t expectedGeneration,
                                  ActivityAuthorityResetSnapshot& output) noexcept {
    output = {};
    const std::shared_lock lock(g_lock);
    std::size_t linkCount = 0;
    const Session* const session = unique_activity_link_locked(binding, linkCount);
    ActivityAuthorityResetStatus status = ActivityAuthorityResetStatus::noActivityLink;
    if (session != nullptr) {
        status = session->activity.bindingGeneration == expectedGeneration
                     ? authority_reset::snapshot(
                           session->activityAuthorityReset, expectedGeneration, output)
                     : ActivityAuthorityResetStatus::staleActivityClient;
    }
    return status;
}

/** Cancels a pending raw incident while excluding activity-link publication. */
bool cancel_activity_host_incident(const state::activity::SessionBinding& binding) noexcept {
    const std::lock_guard lock(g_lock);
    const bool canceled = server::activity::host::cancel_pending_incident(binding);
    return canceled;
}

#if defined(SUNRISE_BAP_FRAME_TEST)
/** Copies one armed connection's own send nonce and session key. Test-only, never shipped. */
bool session_channel(std::uint32_t connectionId,
                     std::array<std::byte, state::kBapNonceSize>& sendNonce,
                     std::array<std::byte, state::kAesKeySize>& sessionKey) noexcept {
    const std::shared_lock lock(g_lock);
    const Session* const session = session_for(connectionId);
    const bool armed = session != nullptr && session->authenticated;
    if (armed) {
        sendNonce = session->sendNonce;
        sessionKey = session->sessionKey;
    }
    return armed;
}
#endif

/** Securely erases every connection-owned nonce and transform buffer. */
void shutdown() noexcept {
    const std::lock_guard lock(g_lock);
    drain_world_rewards();
    for (auto& session : g_sessions) {
        if (session.id != 0
            && session.matchmakingContext.generation != state::matchmaking::kInvalidGeneration) {
            // State erases runtime descriptors before the opaque association is cleared.
            (void)state::matchmaking::release_context(session.matchmakingContext);
        }
        if (session.id != 0) {
            encrypted::release_activity_connection(session);
        }
    }
    SecureZeroMemory(g_sessions.data(), sizeof g_sessions);
    SecureZeroMemory(&g_scratch, sizeof g_scratch);
}

} // namespace sunrise::server::bap
