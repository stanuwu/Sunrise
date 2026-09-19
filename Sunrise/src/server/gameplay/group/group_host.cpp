#include "group_host.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/descriptor/net_addr.h"
#include "../../../middleware/gameplay/group/member_messages.h"
#include "../../../middleware/gameplay/group/parameter_messages.h"
#include "../../../middleware/gameplay/group/parameter_registry.h"
#include "../../../middleware/gameplay/group/session_messages.h"
#include "../../../middleware/gameplay/group/session_state.h"
#include "../../../middleware/gameplay/group/view_message.h"
#include "../../../state/activity/fireteam.h"
#include "../../../state/activity/member_presence.h"
#include "../../../state/activity/membership/activity_transport_fields.h"
#include "../../../state/activity/runtime.h"
#include "../../bap/runtime.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_advertisement.h"
#include "../gameplay_log.h"
#include "../peer/peer_transport.h"
#include "group_host_admission.h"
#include "group_host_internal.h"
#include "group_host_parameters.h"
#include "group_host_sessions.h"
#include "group_membership_compose.h"
#include "group_migration_receipts.h"

namespace sunrise::server::gameplay::group {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;
namespace descriptor = middleware::gameplay::descriptor;

using admission::Admitted;
using admission::claim;
using admission::find_owned;
using admission::g_admitted;
using admission::g_admittedLock;
using admission::kAdmittedCapacity;
using admission::owned_elsewhere;

/** Local membership-body limit, separate from the reliable queue's fragment capacity. */
constexpr std::size_t kMembershipBodyCapacity = state::gameplay::kGroupMessageCapacity;
/** Registry index the join-latch update names. Any index would do; none is ever filled. */
constexpr std::uint8_t kJoinLatchParameter = 0;
/** Revision of the last published snapshot. The consumer refuses one that does not increase. */
std::atomic<std::uint32_t> g_membershipRevision{0};
/** Last carrier change serviced under the admitted lock. */
std::uint64_t g_transportFieldsGeneration{};
/** Member state this host publishes for every member carrying the join id. */
constexpr wire::MemberState kJoinMemberState = wire::MemberState::ready;

// The joining peer's entry must be at least `joined` and must not be `established`, and its request
// waits until every member carrying the join id reads `ready`.
static_assert(static_cast<std::uint8_t>(kJoinMemberState)
                  >= static_cast<std::uint8_t>(wire::MemberState::joined),
              "the published member state must clear the peer's own join bar");
static_assert(kJoinMemberState == wire::MemberState::ready,
              "the request advances only when every member carrying the join id reads ready");

/**
 * Publishes this recipient's snapshot of the native peers admitted to its group.
 * The caller holds the admitted lock.
 * @param record Admitted peer the snapshot names.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_snapshot(Admitted& record) noexcept {
    // Cleared presence is unsampled, not a native report withdrawing the existing players.
    for (const auto& row : g_admitted) {
        if (row.occupied && row.sessionId == record.sessionId && row.hasPlayer
            && row.presencePending) {
            record.rosterStale = true;
            return false;
        }
    }
    const auto host = endpoint::advertised();
    std::array<std::byte, descriptor::kNetAddrSize> hostAddress{};
    const auto hostPort = record.endpoint.localPort != 0 ? record.endpoint.localPort : host.port;
    descriptor::write_net_addr(host.address, hostPort, hostAddress);
    std::array<compose::PeerInput, compose::kPeerCapacity> peers{};
    // A peer with no published carrier reads as a fallback.
    std::array<descriptor::NetAddrNormalisation, compose::kPeerCapacity> identityRule{};
    std::array<std::uint64_t, compose::kPeerCapacity> identityAccounts{};
    std::size_t count = 0;
    for (const auto& row : g_admitted) {
        if (!admission::visible_member(record, row)) {
            continue;
        }
        if (count == peers.size()) {
            return false;
        }
        const auto slot = count;
        auto& input = peers[count++];
        input.recipient = &row == &record;
        identityAccounts[slot] = row.playerSoids.present ? row.playerSoids.accountSoid : 0;
        // The carrier the peer published about itself outranks the address its connect request
        // carried: a second, unreconciled address for the same peer makes the recipient register
        // a second security context and clears the channel's security flag for every packet in
        // both directions. Keyed by account, not `machineId`, because the join request's machine
        // id and the published carrier live in different id spaces, and resolving the account
        // needs no BAP lock, which this path cannot take while already holding the admitted lock.
        if (row.playerSoids.present && row.playerSoids.accountSoid != 0) {
            namespace membership = state::activity::membership;
            membership::TransportFields published{};
            std::array<std::byte, descriptor::kNetAddrSize> chosen{};
            // Exact character first; the account-wide read still refuses when two of its
            // characters published carriers that disagree.
            if (membership::transport_fields_for_account(
                    row.playerSoids.accountSoid, row.playerSoids.characterSoid, published)
                || membership::transport_fields_for_account(
                    row.playerSoids.accountSoid, 0, published)) {
                const auto rule = descriptor::normalize_net_addr_ipv4(
                    published.address, published.addressAlt, chosen);
                if (rule != descriptor::NetAddrNormalisation::unavailable) {
                    input.member.address = chosen;
                    identityRule[slot] = rule;
                }
            }
        }
        if (identityRule[slot] == descriptor::NetAddrNormalisation::unavailable) {
            // A Steam text carrier names no routable IPv4, so a recipient handed one cannot open
            // a direct channel to that peer at all. The link's own endpoint is the routable
            // identity.
            if (!peer::remote_address(row.endpoint, row.sessionId, input.member.address)
                || descriptor::net_addr_is_steam_text(input.member.address)) {
                descriptor::write_net_addr(
                    row.endpoint.address, row.endpoint.port, input.member.address);
            }
        }
        input.member.machineId = row.machineId != 0 ? row.machineId : row.joinId;
        input.member.joinId = row.joinId;
        input.member.state = row.joinComplete ? wire::MemberState::established : kJoinMemberState;
        input.member.connectionPresent = true;
        input.hasPlayer = admission::visible_player(record, row);
        if (input.hasPlayer && row.playerSoids.present
            && !wire::complete_native_player_profile(row.playerProfile)) {
            return false;
        }
        input.player.slot = row.playerSlot;
        input.player.playerId = row.playerId;
        input.player.addSequence = row.playerAddSequence;
        input.player.hasProfile = row.playerSoids.present;
        input.player.profileKind = row.playerKind;
        input.player.profileValue = row.playerProfileValue;
        input.player.accountSoid = row.playerSoids.accountSoid;
        input.player.characterSoid = row.playerSoids.characterSoid;
        input.player.nativeProfile = row.playerProfile;
    }
    compose::Membership composed;
    const auto revision = g_membershipRevision.load() + 1;
    if (!compose::membership(
            record.sessionId, hostAddress, std::span(peers).first(count), revision, composed)) {
        return false;
    }
    // Byte-identical content is not republished; only the revision would differ.
    const auto candidate = PublicationStamp::from(composed.update);
    if (record.publication.matches(candidate)) {
        record.rosterStale = false;
        return true;
    }
    // A peer named by its link endpoint instead of its published carrier is the one case where
    // a different recipient's snapshot can name it by different bytes, so only that case is
    // reported, and only for a snapshot that actually goes on the wire.
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (identityAccounts[slot] != 0
            && identityRule[slot] == descriptor::NetAddrNormalisation::unavailable) {
            report(core::log::Level::warn,
                   "ev=gameplay stage=peer_identity result=fallback account=0x%016llX",
                   static_cast<unsigned long long>(identityAccounts[slot]));
        }
    }
    std::array<std::byte, kMembershipBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!wire::write_membership_update(writer, composed.update) || !writer.finish(size)) {
        return false;
    }
    const bool queued =
        peer::enqueue_reliable(record.endpoint,
                               static_cast<std::uint8_t>(wire::SessionMessageId::membershipUpdate),
                               wire::kMembershipUpdateSize,
                               {body.data(), size},
                               writer.bit_count());
    if (queued) {
        g_membershipRevision.store(revision);
        record.publication = candidate;
        record.rosterStale = false;
    }
    return queued;
}
/**
 * Advances one view-establishment stage and commits only an enqueued response.
 * The binding is keyed by the link because the body names no session.
 * @param from Peer endpoint the view arrived from.
 * @param view Decoded view body.
 */
void bind_view(const state::gameplay::Endpoint& from,
               const wire::ViewEstablishment& view) noexcept {
    HostSessionBinding participantHost{};
    const bool participant = host_session_for_activity(view.sessionToken, participantHost)
                             && from.localPort != 0 && participantHost.port == from.localPort;
    wire::PlayerBlockSoids owner{};
    const bool resolvedOwner =
        admission::view_owner(from, owner, participant ? participantHost.groupSessionId : 0);
    server::bap::ActivityReplicationView activity{};
    const bool ready = resolvedOwner && owner.present
                       && server::bap::activity_replication_view_for_session(
                           view.sessionToken, owner.accountSoid, owner.characterSoid, activity);
    const bool embeddedReady =
        resolvedOwner && !owner.present && core::settings::role() == core::settings::Role::embedded
        && server::bap::activity_replication_view_for_session(view.sessionToken, activity);
    if (ready || embeddedReady) {
        std::uint64_t groupSessionId = activity.groupSessionId;
        HostSessionBinding privateHost{};
        if (groupSessionId == 0
            && server::gameplay::private_host_session(activity.binding, privateHost)) {
            groupSessionId = privateHost.groupSessionId;
        }
        static_cast<void>(peer::open_external_common(from,
                                                     groupSessionId,
                                                     activity.binding,
                                                     activity.patchEpoch,
                                                     activity.activityClientGeneration,
                                                     activity.replicationEpoch));
    }
    wire::ViewEstablishment response{};
    std::uint64_t generation = 0;
    const peer::ViewStageResult result = peer::receive_view_stage(from, view, response, generation);
    const bool sent = result == peer::ViewStageResult::accepted
                      && send_reliable(from,
                                       wire::kViewMessageId,
                                       wire::kViewMessageSize,
                                       [&response](bits::Writer& writer) noexcept {
                                           return wire::write_view(writer, response);
                                       });
    const bool committed = sent && peer::commit_view_response(from, generation);
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=view result=%s remote=%u local=%u token=0x%llX list=%u",
           committed                                   ? "queued"
           : result == peer::ViewStageResult::accepted ? "queue_fail"
                                                       : "refused",
           static_cast<unsigned>(view.kind),
           static_cast<unsigned>(response.kind),
           static_cast<unsigned long long>(view.sessionToken),
           static_cast<unsigned>(view.listCount));
}

/**
 * Answers one time-synchronize request with the three-sample reply.
 * A reply is terminal and gets no answer.
 * @param from Peer endpoint.
 * @param probe Decoded body.
 * @param now Tick the request arrived on.
 */
void answer_time(const state::gameplay::Endpoint& from,
                 const wire::TimeSynchronize& probe,
                 std::uint64_t now) noexcept {
    if (probe.threeSample) {
        return;
    }
    wire::TimeSynchronize reply{};
    reply.sessionId = probe.sessionId;
    reply.threeSample = true;
    reply.requesterSendTime = probe.requesterSendTime;
    reply.responderReceiveTime = now;
    // The peer derives its clock offset from all four stamps, so the send stamp is taken last.
    reply.responderSendTime = GetTickCount64();
    if (!peer::send_out_of_band(from,
                                static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize),
                                wire::kTimeSynchronizeSize,
                                [&reply](bits::Writer& writer) noexcept {
                                    return wire::write_time_synchronize(writer, reply);
                                })) {
        report(core::log::Level::debug, "ev=gameplay stage=time result=fail");
    }
}

/**
 * Drops one session's link and its admitted record together.
 * A leave names one region's session, and the client's other region must keep its own link.
 * @param sessionId Session the peer is leaving.
 */
void drop_session(const state::gameplay::Endpoint& from, std::uint64_t sessionId) noexcept {
    peer::drop(from, sessionId);
    // The region's activity host stays. A leave is also how the peer fast travels to the region it
    // is already in, and a fresh id there is `public_activity_host_mismatch`.
    AcquireSRWLockExclusive(&g_admittedLock);
    static_cast<void>(admission::depart(from, sessionId));
    ReleaseSRWLockExclusive(&g_admittedLock);
}

/**
 * Acknowledges one leave and drops the session it names.
 * @return True when the body was decoded and the container may continue.
 */
[[nodiscard]] bool consume_leave(const state::gameplay::Endpoint& from,
                                 bits::Reader& reader) noexcept {
    std::uint64_t leaving = 0;
    if (!wire::read_session_only(reader, leaving)) {
        return false;
    }
    // A leave tears the session's link down, so a peer must not be able to send one for a session
    // another endpoint was admitted for.
    if (owned_elsewhere(from, leaving)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=leave result=unowned session=0x%016llX",
               static_cast<unsigned long long>(leaving));
        return true;
    }
    const bool sent =
        peer::send_out_of_band(from,
                               static_cast<std::uint8_t>(wire::SessionMessageId::leaveAcknowledge),
                               wire::kLeaveAcknowledgeSize,
                               [leaving](bits::Writer& writer) noexcept {
                                   return wire::write_session_only(writer, leaving);
                               });
    report(core::log::Level::info,
           "ev=gameplay stage=leave result=%s session=0x%016llX",
           sent ? "acknowledged" : "fail",
           static_cast<unsigned long long>(leaving));
    // A failed send keeps the state the peer's repeated leave needs.
    if (sent) {
        drop_session(from, leaving);
    }
    return true;
}

/**
 * Answers one join-completion report with the parameter it still owes and a fresh snapshot.
 * The peer repeats this until its membership shows every member of the join at `established`.
 * @return True when the body was decoded and the container may continue.
 */
[[nodiscard]] bool consume_join_complete(const state::gameplay::Endpoint& from,
                                         bits::Reader& reader) noexcept {
    wire::JoinComplete body{};
    if (!wire::read_join_complete(reader, body)) {
        return false;
    }
    // Keyed by the body's session, not the link's: one link carries every region the client joined
    // over it.
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = find_owned(from, body.sessionId);
    bool queued = false;
    if (record != nullptr) {
        // The state change the report implies. The parameter follows that change, not the report,
        // so a repeat does not publish it twice.
        const bool joined = !record->joinComplete;
        record->joinComplete = true;
        // Before the snapshot, because it is the smaller message. A queue that then refuses the
        // snapshot is answered by the peer's next repeat of this same report.
        if (joined) {
            admission::mark_stale(record->sessionId);
            record->parameterOwed =
                !publish_activity_host(from, record->sessionId, admission::member_mask(*record));
        }
        queued = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    report(queued ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=join result=%s session=0x%llX machine=0x%llX update=%u",
           queued              ? "completed"
           : record == nullptr ? "fail"
                               : "deferred",
           static_cast<unsigned long long>(body.sessionId),
           static_cast<unsigned long long>(body.machineId),
           body.joinSequence);
    return true;
}

/**
 * Drops the session one abandoned join names.
 * @return True when the body was decoded and the container may continue.
 */
[[nodiscard]] bool consume_join_abort(const state::gameplay::Endpoint& from,
                                      bits::Reader& reader) noexcept {
    wire::SessionNotice notice{};
    if (!wire::read_join_abort(reader, notice)) {
        return false;
    }
    if (owned_elsewhere(from, notice.sessionId)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=join result=unowned_abort session=0x%016llX",
               static_cast<unsigned long long>(notice.sessionId));
        return true;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=join result=abort session=0x%016llX",
           static_cast<unsigned long long>(notice.sessionId));
    drop_session(from, notice.sessionId);
    return true;
}

/**
 * Answers one parameter request with every requested parameter this host can encode.
 * @return True only when every selected body was located, which is what leaves the container
 *         readable behind the request.
 */
[[nodiscard]] bool consume_parameter_request(const state::gameplay::Endpoint& from,
                                             bits::Reader& reader) noexcept {
    wire::ParameterRequestHeader header{};
    if (!wire::read_parameter_request(reader, header)) {
        return false;
    }
    const std::uint64_t mask = header.requestedMask & wire::kParameterMaskBits;
    std::array<char, kParameterNameCapacity> names{};
    report(core::log::Level::info,
           "ev=gameplay stage=parameters result=request mask=0x%08X mode=%u names=%s",
           static_cast<unsigned>(mask),
           static_cast<unsigned>(header.modeFlag ? 1U : 0U),
           wire::parameter_names(mask, names.data(), names.size()));
    // Locate the selected bodies to determine whether the following container remains readable.
    // An incomplete walk still permits the structural reply below, then stops container parsing.
    wire::ParameterRequestWalk walk{};
    const bool intact = wire::walk_parameter_request(reader, mask, walk);
    report(walk.complete ? core::log::Level::debug : core::log::Level::info,
           "ev=gameplay stage=parameters result=%s walked=0x%08X stopped=%u tail=%u",
           walk.complete ? "framed"
           : intact      ? "ambiguous"
                         : "truncated",
           static_cast<unsigned>(walk.walkedMask),
           static_cast<unsigned>(walk.ambiguousParameter),
           walk.tailBits);
    // The peer builds no activity client until it holds the host parameter, so the answer goes out
    // even when a later body could not be located. A session another endpoint holds is answered by
    // that endpoint.
    if (!owned_elsewhere(from, header.sessionId)) {
        AcquireSRWLockShared(&g_admittedLock);
        const auto* recipient = admission::find(from, header.sessionId);
        const auto memberMask = recipient ? admission::member_mask(*recipient) : 1U;
        const auto players = recipient ? admission::player_count(*recipient) : std::uint8_t{0};
        ReleaseSRWLockShared(&g_admittedLock);
        answer_parameters(from, header.sessionId, mask, players, memberMask);
    }
    return walk.complete;
}

/**
 * Adopts the player one peer asked this host to add and republishes the snapshot.
 * @return True when the complete native body was decoded.
 */
[[nodiscard]] bool consume_player_add(const state::gameplay::Endpoint& from,
                                      bits::Reader& reader) noexcept {
    wire::PlayerAddRequest request{};
    if (!wire::read_player_add(reader, request)) {
        return false;
    }
    // Retain identity only from this endpoint's own native player-add.
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = find_owned(from, request.sessionId);
    bool published = false;
    const bool accepted = record != nullptr && admission::set_player(*record, request);
    if (accepted) {
        published = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    report(core::log::Level::info,
           "ev=gameplay stage=player result=%s session=0x%llX player=0x%llX seq=%u kind=%u "
           "acct=0x%llX character=0x%llX published=%u",
           accepted ? "added" : "fail",
           static_cast<unsigned long long>(request.sessionId),
           static_cast<unsigned long long>(request.playerId),
           request.sequence,
           static_cast<unsigned>(request.kind),
           static_cast<unsigned long long>(request.soids.accountSoid),
           static_cast<unsigned long long>(request.soids.characterSoid),
           published ? 1U : 0U);
    return true;
}

/**
 * Drops the player the named session's record holds. The message names no player itself.
 * @return True when the body was decoded. The body is two fields, so the container continues.
 */
[[nodiscard]] bool consume_player_remove(const state::gameplay::Endpoint& from,
                                         bits::Reader& reader) noexcept {
    wire::PlayerRemoveRequest request{};
    if (!wire::read_player_remove(reader, request)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = find_owned(from, request.sessionId);
    bool published = false;
    if (record != nullptr && admission::clear_player(*record)) {
        published = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    report(core::log::Level::info,
           "ev=gameplay stage=player result=%s session=0x%llX",
           published           ? "removed"
           : record == nullptr ? "fail"
                               : "absent",
           static_cast<unsigned long long>(request.sessionId));
    return true;
}

} // namespace

void refresh_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    admission::refresh_endpoint(endpoint);
    ReleaseSRWLockExclusive(&g_admittedLock);
}

/** Frees every admitted record at one endpoint. */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.endpoint == endpoint) {
            ++count;
            static_cast<void>(admission::depart(endpoint, entry.sessionId));
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    if (count != 0) {
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=dropped endpoint=0x%08X:%u sessions=%zu",
               endpoint.address,
               static_cast<unsigned>(endpoint.port),
               count);
    }
}

void release_account(std::uint64_t accountSoid) noexcept {
    if (!accountSoid) {
        return;
    }
    AcquireSRWLockExclusive(&g_admittedLock);
    for (auto& record : g_admitted) {
        if (record.occupied && record.hasPlayer && record.playerSoids.present
            && record.playerSoids.accountSoid == accountSoid) {
            // Native channel closure can preserve a group for a channel rebuild. Losing the
            // account's last authenticated link ends that ownership instead; otherwise a fresh
            // endpoint leaves duplicate machine/player identities in every membership snapshot.
            static_cast<void>(admission::depart(record.endpoint, record.sessionId));
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
}

/** Consumes one group-session message. */
bool consume(const state::gameplay::Endpoint& from,
             std::uint8_t id,
             bits::Reader& reader,
             std::uint64_t now) noexcept {
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize)) {
        wire::TimeSynchronize probe{};
        if (!wire::read_time_synchronize(reader, probe)) {
            return false;
        }
        answer_time(from, probe, now);
        return true;
    }
    if (id == wire::kViewMessageId) {
        wire::ViewEstablishment view{};
        if (!wire::read_view(reader, view)) {
            return false;
        }
        bind_view(from, view);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::leaveSession)) {
        return consume_leave(from, reader);
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::peerEstablish)) {
        std::uint64_t established = 0;
        if (!wire::read_session_only(reader, established)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=establish result=ok session=0x%016llX",
               static_cast<unsigned long long>(established));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinComplete)) {
        return consume_join_complete(from, reader);
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinAbort)) {
        return consume_join_abort(from, reader);
    }
    if (id == wire::kParameterRequestId) {
        return consume_parameter_request(from, reader);
    }
    if (id == wire::kPeerPropertiesId) {
        wire::PeerPropertiesHeader header{};
        if (!wire::read_peer_properties_header(reader, header)) {
            return false;
        }
        // The 304-byte property block behind the address is not decoded, so the body is
        // reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=properties result=read session=0x%llX method=%u",
               static_cast<unsigned long long>(header.sessionId),
               static_cast<unsigned>(header.addressMethod));
        return false;
    }
    if (id == wire::kPlayerAddId) {
        return consume_player_add(from, reader);
    }
    if (id == wire::kPlayerRemoveId) {
        return consume_player_remove(from, reader);
    }
    if (id == wire::kPlayerPropertiesId) {
        wire::PlayerPropertiesRequest request{};
        if (!wire::read_player_properties(reader, request)) {
            return false;
        }
        AcquireSRWLockExclusive(&g_admittedLock);
        auto* record = find_owned(from, request.sessionId);
        const bool accepted = record && admission::update_player(*record, request);
        if (accepted) {
            (void)publish_snapshot(*record);
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(
            core::log::Level::info,
            "ev=gameplay stage=player result=properties session=0x%llX seq=%u kind=%u accepted=%u",
            static_cast<unsigned long long>(request.sessionId),
            request.sequence,
            static_cast<unsigned>(request.kind),
            accepted ? 1U : 0U);
        return true;
    }
    return migration::consume(from, id, reader);
}

/** Publishes the membership snapshot that completes one peer's join. */
bool publish_membership(const state::gameplay::Endpoint& peer,
                        std::uint64_t peerJoinId,
                        std::uint64_t peerMachineId,
                        std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = claim(peer, sessionId);
    bool published = false;
    if (record != nullptr) {
        // A retried join brings a new join id and drops any player the previous attempt added.
        // It also starts again at `ready`, so the previous attempt's completion does not carry.
        const bool changed = record->joinId != peerJoinId || record->machineId != peerMachineId;
        if (changed) {
            record->publication = {};
            record->presence = {};
            static_cast<void>(admission::clear_player(*record));
            record->joinComplete = false;
            record->parameterOwed = false;
        }
        record->joinId = peerJoinId;
        record->machineId = peerMachineId;
        record->sessionId = sessionId;
        admission::mark_stale(sessionId);
        published = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    return published;
}

/** Checks whether one endpoint owns an admitted session row. */
bool admitted_owner(const state::gameplay::Endpoint& endpoint, std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_admittedLock);
    const bool owned = admission::find(endpoint, sessionId) != nullptr;
    ReleaseSRWLockShared(&g_admittedLock);
    return owned;
}

/** Retries pending recipient publications and retires only excess sessions of one endpoint. */
void service(std::uint64_t) noexcept {
    allocate_claimed_host_sessions();
    Admitted retired{};
    std::array<Admitted, kAdmittedCapacity> active{};
    std::size_t activeCount = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    const auto transportGeneration = state::activity::membership::transport_fields_generation();
    if (transportGeneration != g_transportFieldsGeneration) {
        // The mirror's own lock keeps this independent of BAP. Snapshot stamps suppress groups
        // whose bytes did not change; a refused publication keeps its roster debt for retry.
        for (auto& record : g_admitted) {
            if (record.occupied) {
                record.rosterStale = true;
            }
        }
        g_transportFieldsGeneration = transportGeneration;
    }
    const bool hasRetired = admission::retire_excess(retired);
    for (const auto& record : g_admitted) {
        if (record.occupied) {
            active[activeCount++] = record;
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    std::array<residency::Report, kAdmittedCapacity> reports{};
    for (std::size_t index = 0; index < activeCount; ++index) {
        const auto& record = active[index];
        auto& report = reports[index];
        HostSessionBinding host{};
        if (host_session_for_group(record.sessionId, host)) {
            report.sessionRegion = host.regionIndex;
            if (record.hasPlayer && record.playerSoids.present) {
                state::activity::membership::ClientPlacement placement{};
                if (state::activity::presence::placement(host.target,
                                                         record.playerSoids.accountSoid,
                                                         record.playerSoids.characterSoid,
                                                         placement)) {
                    report.currentRegion = placement.currentRegion;
                }
            }
        }
        if (record.hasPlayer && record.playerSoids.present) {
            state::activity::membership::ClientPlacement placement{};
            if (state::activity::presence::latest_placement(
                    record.playerSoids.accountSoid, record.playerSoids.characterSoid, placement)) {
                report.pendingRegion = placement.region;
            }
            std::size_t connected = 0;
            for (std::size_t other = 0; other < activeCount; ++other) {
                const auto& peer = active[other];
                if (peer.sessionId == record.sessionId && peer.playerSoids.present
                    && connected < report.fireteamAccounts.size()
                    && state::activity::fireteam::connected(record.playerSoids.accountSoid,
                                                            peer.playerSoids.accountSoid)) {
                    report.fireteamAccounts[connected++] = peer.playerSoids.accountSoid;
                }
            }
        }
        server::bap::ActivityReplicationView view{};
        const bool ready =
            record.hasPlayer && record.playerSoids.present
            && server::bap::activity_replication_view_for_group(record.sessionId,
                                                                record.playerSoids.accountSoid,
                                                                record.playerSoids.characterSoid,
                                                                view);
        const bool embeddedReady =
            !record.playerSoids.present && core::settings::role() == core::settings::Role::embedded
            && server::bap::activity_replication_view_for_group(record.sessionId, view);
        if (ready || embeddedReady) {
            static_cast<void>(peer::open_external_common(record.endpoint,
                                                         view.groupSessionId,
                                                         view.binding,
                                                         view.patchEpoch,
                                                         view.activityClientGeneration,
                                                         view.replicationEpoch));
        }
    }
    AcquireSRWLockExclusive(&g_admittedLock);
    for (std::size_t index = 0; index < activeCount; ++index) {
        const auto& sampled = active[index];
        auto* record = admission::find(sampled.endpoint, sampled.sessionId);
        // A concurrent native rejoin or player switch invalidates the sampled identity.
        if (!record || record->joinId != sampled.joinId || record->machineId != sampled.machineId
            || record->playerId != sampled.playerId || record->hasPlayer != sampled.hasPlayer
            || record->playerSoids.accountSoid != sampled.playerSoids.accountSoid
            || record->playerSoids.characterSoid != sampled.playerSoids.characterSoid) {
            continue;
        }
        if (record->presencePending || record->presence != reports[index]) {
            record->presence = reports[index];
            record->presencePending = false;
            admission::mark_stale(record->sessionId);
        }
    }
    for (auto& record : g_admitted) {
        if (!record.occupied) {
            continue;
        }
        if (record.rosterStale) {
            static_cast<void>(publish_snapshot(record));
        }
        if (record.parameterOwed) {
            record.parameterOwed = !publish_activity_host(
                record.endpoint, record.sessionId, admission::member_mask(record));
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    if (hasRetired) {
        peer::drop(retired.endpoint, retired.sessionId);
    }
}
/** Publishes the parameter update a joining peer needs before it will finish its join. */
bool publish_join_parameters(const state::gameplay::Endpoint& endpoint,
                             std::uint64_t sessionId) noexcept {
    // A joining peer finishes only once it has applied one update, whatever it names. Releasing a
    // slot the peer never filled sets that latch and leaves the peer's state alone.
    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.releasedMask = std::uint64_t{1} << kJoinLatchParameter;

    const bool sent = send_parameter_update(update, endpoint);
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s released=0x%08X names=%s",
           sent ? "queued" : "fail",
           static_cast<unsigned>(update.releasedMask),
           wire::parameter_names(update.releasedMask, names.data(), names.size()));
    return sent;
}

/** Reports whether replication may produce entity output for one peer. */
bool view_accepted(std::uint64_t sessionId) noexcept {
    return peer::view_bound(sessionId);
}

/** Copies every admitted group-session record. */
void snapshot_admitted(std::span<AdmittedRow> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_admittedLock);
    for (const Admitted& entry : g_admitted) {
        if (!entry.occupied || count >= output.size()) {
            continue;
        }
        output[count] = {entry.sessionId,
                         entry.endpoint,
                         entry.joinComplete,
                         entry.joinComplete && !entry.parameterOwed,
                         entry.hasPlayer,
                         entry.joinId};
        ++count;
    }
    ReleaseSRWLockShared(&g_admittedLock);
}

/** Clears every group-session record. */
void reset() noexcept {
    g_membershipRevision.store(0);
    // Every host session goes back to State as well, or its records are stranded there.
    reset_host_sessions();
    AcquireSRWLockExclusive(&g_admittedLock);
    g_admitted = {};
    g_transportFieldsGeneration = 0;
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace sunrise::server::gameplay::group
