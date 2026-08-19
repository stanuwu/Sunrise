#include "group_host.h"

#include <Windows.h>

#include <array>
#include <atomic>

#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/group/member_messages.h"
#include "../../../middleware/gameplay/group/parameter_messages.h"
#include "../../../middleware/gameplay/group/parameter_registry.h"
#include "../../../middleware/gameplay/group/session_messages.h"
#include "../../../middleware/gameplay/group/session_state.h"
#include "../../../middleware/gameplay/group/view_message.h"
#include "../../../state/activity/runtime.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../peer/peer_transport.h"
#include "group_host_sessions.h"
#include "group_migration_receipts.h"

namespace sunrise::server::gameplay::group {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;
namespace descriptor = middleware::gameplay::descriptor;

/** One reliable body staged before it is split into fragments. */
constexpr std::size_t kBodyCapacity = 128;
/** A membership snapshot is far larger, and the peer's reliable send queue bounds it. */
constexpr std::size_t kMembershipBodyCapacity = 512;
/** Only the low 25 bitmap bits name a registry parameter. */
constexpr std::uint64_t kParameterMaskBits = 0x1FFFFFF;
/** Room for every registry name plus its separators. */
constexpr std::size_t kParameterNameCapacity = 640;
/** Member index this host takes, and the index it nominates to succeed it. */
constexpr std::uint32_t kHostMemberIndex = 0;
/** Member index the admitted peer takes. */
constexpr std::size_t kPeerMemberIndex = 1;
/** Members one snapshot names: this host and the admitted peer. */
constexpr std::size_t kSnapshotMemberCount = 2;
/** Registry index the join-latch update names. Any of the 25 would do; none is ever filled. */
constexpr std::uint8_t kJoinLatchParameter = 0;
/** Peers this host tracks at once. The public POC admits one. */
constexpr std::size_t kAdmittedCapacity = 4;
/** Loopback address the BAP listener binds, in host order. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** Every member index the `activity-host` parameter covers. The peer needs its own bit set. */
constexpr std::uint32_t kAllMembers = 0xFFFFFFFF;
/** Shortest gap between two retries of an owed publish. */
constexpr std::uint64_t kRetryInterval = 250;
/** Player slot the admitted peer's player takes. */
constexpr std::uint32_t kPeerPlayerSlot = 0;
/** Counter the first player of a session carries. The consumer's own add starts here too. */
constexpr std::uint32_t kFirstAddSequence = 0;

/** One admitted peer and the player it asked this host to add. */
struct Admitted {
    state::gameplay::Endpoint endpoint{};
    std::uint64_t joinId{};
    std::uint64_t playerId{};
    /** Group-session id the peer named in its join request, which its parameters must echo. */
    std::uint64_t sessionId{};
    bool occupied{};
    bool hasPlayer{};
    /** Set once the peer reports its join finished, which is what promotes it to `established`. */
    bool joinComplete{};
    /** Set once a snapshot carrying that promotion is on the peer's reliable channel. */
    bool joinPublished{};
    /** Set once the `activity-host` parameter is on the peer's reliable channel. */
    bool activityHostPublished{};
    /** Set once a snapshot naming the peer's player is on that channel. The queue can refuse it. */
    bool playerPublished{};
    /** Tick of the last retry, so a full queue is retried on a timer rather than every packet. */
    std::uint64_t lastRetry{};
    /** Order in which the peer last named this session. The lowest is the least recently used. */
    std::uint64_t lastUse{};
};

/**
 * Public group sessions the peer holds at once: one current and one target.
 * The peer resolves a session through a two-element array, so a third is one it left.
 */
constexpr std::size_t kPublicSessionCapacity = 2;

/** Revision of the last published snapshot. The consumer refuses one that does not increase. */
std::atomic<std::uint32_t> g_membershipRevision{0};
/** Stamps `Admitted::lastUse`. It only has to order the records, so it never has to be a clock. */
std::atomic<std::uint64_t> g_admitClock{0};
/** Guards the admitted table against the worker and the callback pump. */
SRWLOCK g_admittedLock{SRWLOCK_INIT};
/** Admitted peers. A join claims a slot and a leave never reclaims one in this POC. */
std::array<Admitted, kAdmittedCapacity> g_admitted{};

/** Member state this host publishes for every member carrying the join id. */
constexpr wire::MemberState kJoinMemberState = wire::MemberState::ready;

// Three peer checks pin this to exactly `ready`. The joining peer's entry must be at least
// `joined`, must not be `established`, and the request waits until every member carrying the
// join id reads `ready`.
static_assert(static_cast<std::uint8_t>(kJoinMemberState)
                  >= static_cast<std::uint8_t>(wire::MemberState::joined),
              "the published member state must clear the peer's own join bar");
static_assert(kJoinMemberState == wire::MemberState::ready,
              "the request advances only when every member carrying the join id reads ready");

/**
 * Sends one reliable group-session message.
 * @param sessionId Group session whose reliable channel carries it.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares.
 * @param write Callback that writes the body.
 * @return True when the message was queued.
 */
template <typename Body>
[[nodiscard]] bool send_reliable(std::uint64_t sessionId,
                                 std::uint8_t id,
                                 std::uint32_t declaredSize,
                                 Body write) noexcept {
    std::array<std::byte, kBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!write(writer) || !writer.finish(size)) {
        return false;
    }
    return peer::enqueue_reliable(
        sessionId, id, declaredSize, {body.data(), size}, writer.bit_count());
}

/** @return True when two endpoints name the same address and port. */
[[nodiscard]] bool same_endpoint(const state::gameplay::Endpoint& left,
                                 const state::gameplay::Endpoint& right) noexcept {
    return left.address == right.address && left.port == right.port;
}

/**
 * Finds or claims the record for one peer, and binds it to that peer's endpoint.
 * Admission is what establishes ownership, so this rebinds an existing record. A client that
 * rebuilds its channel arrives from a new port and joins the same session again.
 * @param peer Peer endpoint.
 * @param sessionId Group session the record is keyed by. Zero claims nothing.
 * @return Record for that session, or null when the table is full.
 */
[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    // Keyed by session, not endpoint: one client holds a record per public region and both records
    // name the same endpoint.
    const std::uint64_t use = g_admitClock.fetch_add(1) + 1;
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry.endpoint = peer;
            entry.lastUse = use;
            return &entry;
        }
    }
    for (Admitted& entry : g_admitted) {
        if (!entry.occupied) {
            entry.occupied = true;
            entry.endpoint = peer;
            entry.sessionId = sessionId;
            entry.lastUse = use;
            return &entry;
        }
    }
    return nullptr;
}

/**
 * Finds the record for one session and proves the sender owns it.
 * Every later message names its own session in its body, so without this a peer could name a
 * session another endpoint was admitted for and move that session's state.
 * @param peer Peer endpoint the message arrived from.
 * @param sessionId Group session the message named.
 * @return Record for that session, or null when it is absent or owned by another endpoint.
 */
[[nodiscard]] Admitted* claim_owned(const state::gameplay::Endpoint& peer,
                                    std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    const std::uint64_t use = g_admitClock.fetch_add(1) + 1;
    for (Admitted& entry : g_admitted) {
        if (!entry.occupied || entry.sessionId != sessionId) {
            continue;
        }
        if (!same_endpoint(entry.endpoint, peer)) {
            return nullptr;
        }
        entry.lastUse = use;
        return &entry;
    }
    return nullptr;
}

/**
 * Tests whether another endpoint was admitted for one session.
 * An absent record is not a conflict: a message may name a session before this host has a record
 * for it, and refusing that would strand the peer. A record held elsewhere is a conflict.
 * @param peer Peer endpoint the message arrived from.
 * @param sessionId Group session the message named.
 * @return True when a record holds that session for a different endpoint.
 */
[[nodiscard]] bool owned_elsewhere(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_admittedLock);
    bool conflict = false;
    for (const Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            conflict = !same_endpoint(entry.endpoint, peer);
            break;
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return conflict;
}

/**
 * Publishes one snapshot naming this host, one admitted peer, and that peer's player if it has
 * one. The caller holds the admitted lock.
 * @param record Admitted peer the snapshot names.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_snapshot(const Admitted& record) noexcept {
    const state::gameplay::Endpoint host = endpoint::advertised();
    std::array<wire::MembershipMember, kSnapshotMemberCount> members{};
    descriptor::write_net_addr(host.address, host.port, members[kHostMemberIndex].address);
    // The session id is the machine id this region's descriptor advertised, and the client joined
    // through it. The whole-process identity would name a host this session never saw.
    members[kHostMemberIndex].machineId = record.sessionId;
    // The consumer refuses a table with no entry it recognises as itself, so the peer's own blob is
    // echoed. A blob rebuilt from the endpoint it arrived from is not the same bytes.
    if (!peer::remote_address(record.sessionId, members[kPeerMemberIndex].address)) {
        descriptor::write_net_addr(
            record.endpoint.address, record.endpoint.port, members[kPeerMemberIndex].address);
    }
    // The peer refuses a snapshot whose entry for itself carries another join id. Its real
    // machine id sits in the address table this host does not decode, so the join id stands in.
    members[kPeerMemberIndex].machineId = record.joinId;
    members[kPeerMemberIndex].joinId = record.joinId;
    // The peer ends its join request once no session holds more than one member with that id, so
    // both entries carry it. A table naming it once says the join is over.
    members[kHostMemberIndex].joinId = record.joinId;
    for (wire::MembershipMember& member : members) {
        // The connection group is what makes the consumer resolve the member's peer link. This
        // host has no value for join compatibility or the join timestamp, so both stay cleared.
        member.connectionPresent = true;
    }
    // Both entries carry the join id, so both take the same state. Once the peer reports its join
    // finished they move to `established`, which is what stops it re-sending that report.
    const wire::MemberState state =
        record.joinComplete ? wire::MemberState::established : kJoinMemberState;
    members[kHostMemberIndex].state = state;
    members[kPeerMemberIndex].state = state;

    std::array<wire::MembershipPlayer, 1> players{};
    players[0].slot = kPeerPlayerSlot;
    players[0].playerId = record.playerId;
    players[0].memberIndex = static_cast<std::uint32_t>(kPeerMemberIndex);
    players[0].addSequence = kFirstAddSequence;
    if (record.hasPlayer) {
        members[kPeerMemberIndex].ownsPlayerSlot = true;
        members[kPeerMemberIndex].playerSlot = kPeerPlayerSlot;
    }

    wire::MembershipUpdate update{};
    // The same per-region machine id the member table carries.
    update.hostMachineId = record.sessionId;
    update.revision = g_membershipRevision.fetch_add(1) + 1;
    update.hostMemberIndex = kHostMemberIndex;
    update.successionIndex = kHostMemberIndex;
    update.members = members;
    if (record.hasPlayer) {
        update.players = players;
    }

    std::array<std::byte, kMembershipBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!wire::write_membership_update(writer, update) || !writer.finish(size)) {
        return false;
    }
    // The peer logs the hash it wanted, so ours has to be logged next to it to read a mismatch.
    report(core::log::Level::info,
           "ev=gameplay stage=membership result=built revision=%u members=%zu players=%zu "
           "hash=0x%08X",
           update.revision,
           update.members.size(),
           update.players.size(),
           wire::session_state_hash(update));
    return peer::enqueue_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::SessionMessageId::membershipUpdate),
        wire::kMembershipUpdateSize,
        {body.data(), size},
        writer.bit_count());
}

/**
 * Fills the `activity-host` body this host publishes.
 * The peer creates no activity client until it holds this parameter, and the public-region
 * slice-set switch waits behind that client.
 * @param body Cleared body to fill.
 * @param binding Retained host row used for this whole parameter body.
 */
void fill_activity_host(wire::ActivityHostParameter& body,
                        const HostSessionBinding& binding) noexcept {
    // The peer's `current-activity` carries this host's empty delta, so its nonce is the
    // descriptor default and the comparand is the empty id.
    body.selectionId = 0;
    // The peer addresses its activity join request to this id, and the activity route refuses one
    // that names no committed activity session. A gameplay identity is not one.
    body.hostId = binding.target.sessionId;
    // The peer tests only the bit for its own member index, and this host does not decode which
    // index that is, so every bit is set.
    body.memberMask = kAllMembers;
    body.address = kLoopbackAddress;
    body.port = core::settings::get().server.bapPort;
}

/**
 * Publishes the `activity-host` parameter for one admitted peer. The caller holds the lock.
 * @param record Admitted peer the parameter is published to.
 * @return True when the update was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_activity_host(const Admitted& record) noexcept {
    // The body is built from this copy, so no retain is needed: `host_session_for_group` already
    // returns only a ready row whose State bindings still match, and nothing below reads the table.
    HostSessionBinding binding{};
    if (!host_session_for_group(record.sessionId, binding)) {
        // Publishing a zero host id latches an unusable parameter on the peer, and the peer only
        // reads it once. The region's advertisement allocates and this retries.
        report(core::log::Level::debug, "ev=gameplay stage=activityhost result=nosession");
        return false;
    }
    wire::ParameterUpdate update{};
    update.sessionId = record.sessionId;
    // Both go in one update, so the peer never holds the host without the activity it belongs to.
    // `current-activity` carries an empty delta, which leaves the peer's own descriptor defaults.
    update.carriedMask =
        (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost))
        | (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::currentActivity));
    fill_activity_host(update.activityHost, binding);

    const bool sent = send_reliable(
        record.sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=activityhost result=%s selection=0x%llX host=0x%llX "
           "source_activity=%d target_activity=%d descriptor_bits=%u address=0x%08X port=%u "
           "names=%s",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(update.activityHost.selectionId),
           static_cast<unsigned long long>(update.activityHost.hostId),
           static_cast<int>(binding.source.destination.activityIndex),
           static_cast<int>(binding.target.destination.activityIndex),
           static_cast<unsigned>(binding.target.destination.descriptorBitLength),
           update.activityHost.address,
           static_cast<unsigned>(update.activityHost.port),
           wire::parameter_names(update.carriedMask, names.data(), names.size()));
    return sent;
}

/**
 * Answers one view establishment by binding and echoing the peer's own signature.
 * What a host's own view should hold is unknown. Echoing is the only answer that cannot produce
 * a signature mismatch. The binding is keyed by the link, because the body names no session.
 * @param from Peer endpoint the view arrived from.
 * @param sessionId Session the reply rides back on, or zero when the link carries several.
 * @param view Decoded view body.
 */
void bind_view(const state::gameplay::Endpoint& from,
               std::uint64_t sessionId,
               const wire::ViewEstablishment& view) noexcept {
    state::gameplay::ViewSignature signature{};
    signature.token = view.sessionToken;
    signature.kind = view.kind;
    signature.listCount = view.listCount;
    signature.hasList = view.hasList;
    signature.list = view.list;
    // Kept unread. Its meaning is unrecovered, and dropping it would lose a field the peer sent.
    signature.optionalValue = view.optionalValue;
    signature.hasOptionalValue = view.hasOptionalValue;
    signature.bound = true;
    peer::bind_view(from, signature);

    const bool sent = send_reliable(
        sessionId,
        wire::kViewMessageId,
        wire::kViewMessageSize,
        [&view](bits::Writer& writer) noexcept { return wire::write_view(writer, view); });
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=view result=%s kind=%u token=0x%llX list=%u",
           sent ? "bound" : "fail",
           static_cast<unsigned>(view.kind),
           static_cast<unsigned long long>(view.sessionToken),
           static_cast<unsigned>(view.listCount));
}

/**
 * Answers one parameter request with the parameters this host can encode.
 * An empty answer leaves the peer waiting, so the answer carries every requested parameter that
 * has an encoder and names the rest as unheld.
 * @param sessionId Session the request named, which is also the link it goes back on.
 * @param requested Requested parameter mask, already reduced to its meaningful bits.
 */
void answer_parameters(std::uint64_t sessionId, std::uint64_t requested) noexcept {
    std::uint64_t carried = requested & wire::kEncodableParameters;
    const std::uint64_t activityHostMask =
        std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost);
    // The body is built from this copy, so no retain is needed. See publish_activity_host.
    HostSessionBinding binding{};
    const bool hasHost =
        (carried & activityHostMask) != 0 && host_session_for_group(sessionId, binding);
    if ((carried & activityHostMask) != 0 && !hasHost) {
        // A zero host id is worse than no answer for this one.
        carried &= ~activityHostMask;
    }
    if (carried == 0) {
        report(core::log::Level::debug,
               "ev=gameplay stage=parameters result=unheld mask=0x%08X",
               static_cast<unsigned>(requested));
        return;
    }

    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.carriedMask = carried;
    // A zero host id latches an unusable parameter on the peer, so the answer carries the same
    // body the unsolicited publish does.
    if (hasHost) {
        fill_activity_host(update.activityHost, binding);
    }

    const bool sent = send_reliable(
        sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s carried=0x%08X names=%s",
           sent ? "answered" : "fail",
           static_cast<unsigned>(carried),
           wire::parameter_names(carried, names.data(), names.size()));
}

/**
 * Answers one time-synchronize probe with the same form it arrived in.
 * @param from Peer endpoint.
 * @param probe Decoded probe.
 */
void answer_time(const state::gameplay::Endpoint& from,
                 const wire::TimeSynchronize& probe) noexcept {
    // The exchange must never block the event loop, so the samples are echoed unchanged.
    if (!peer::send_out_of_band(from,
                                static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize),
                                wire::kTimeSynchronizeSize,
                                [&probe](bits::Writer& writer) noexcept {
                                    return wire::write_time_synchronize(writer, probe);
                                })) {
        report(core::log::Level::debug, "ev=gameplay stage=time result=fail");
    }
}

/**
 * Drops one session's link and its admitted record together.
 * A leave names one region's session, and the client's other region must keep its own link.
 * @param sessionId Session the peer is leaving.
 */
void release(std::uint64_t sessionId) noexcept {
    peer::drop(sessionId);
    // The region's activity host stays. A leave is also how the peer fast travels to the region it
    // is already in, and a fresh id there is `public_activity_host_mismatch`.
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace

/** Frees every admitted record at one endpoint. */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && same_endpoint(entry.endpoint, endpoint)) {
            ++count;
            entry = {};
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

/** Consumes one group-session message. */
bool consume(const state::gameplay::Endpoint& from,
             std::uint64_t sessionId,
             std::uint8_t id,
             bits::Reader& reader,
             std::uint64_t now) noexcept {
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize)) {
        wire::TimeSynchronize probe{};
        if (!wire::read_time_synchronize(reader, probe)) {
            return false;
        }
        answer_time(from, probe);
        return true;
    }
    if (id == wire::kViewMessageId) {
        wire::ViewEstablishment view{};
        if (!wire::read_view(reader, view)) {
            return false;
        }
        bind_view(from, sessionId, view);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::leaveSession)) {
        std::uint64_t leaving = 0;
        if (!wire::read_session_only(reader, leaving)) {
            return false;
        }
        if (owned_elsewhere(from, leaving)) {
            // A leave tears the session's link down, so a peer must not be able to send one for a
            // session another endpoint was admitted for.
            report(core::log::Level::warn,
                   "ev=gameplay stage=leave result=unowned session=0x%016llX",
                   static_cast<unsigned long long>(leaving));
            return true;
        }
        const bool sent = peer::send_out_of_band(
            from,
            static_cast<std::uint8_t>(wire::SessionMessageId::leaveAcknowledge),
            wire::kLeaveAcknowledgeSize,
            [leaving](bits::Writer& writer) noexcept {
                return wire::write_session_only(writer, leaving);
            });
        report(core::log::Level::info,
               "ev=gameplay stage=leave result=%s session=0x%016llX",
               sent ? "acknowledged" : "fail",
               static_cast<unsigned long long>(leaving));
        release(leaving);
        return true;
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
        wire::JoinComplete body{};
        if (!wire::read_join_complete(reader, body)) {
            return false;
        }
        // The peer repeats this until its membership shows every member of the join at
        // `established`, so the answer is a snapshot that promotes them. Keyed by the body's
        // session, not the link's: one link carries every region the client joined over it.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, body.sessionId);
        bool queued = false;
        const bool owed = record != nullptr && !record->joinPublished;
        if (record != nullptr) {
            record->joinComplete = true;
            if (owed) {
                queued = publish_snapshot(*record);
                record->joinPublished = queued;
            }
            // The peer only reads the parameter once its join is finished, and the queue is at its
            // fullest right here, so a refusal is expected and the service slice retries it.
            if (record->joinPublished && !record->activityHostPublished) {
                record->activityHostPublished = publish_activity_host(*record);
                record->lastRetry = now;
            }
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(queued ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=join result=%s session=0x%llX machine=0x%llX update=%u",
               queued              ? "completed"
               : record == nullptr ? "fail"
               : owed              ? "deferred"
                                   : "repeat",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.machineId),
               body.joinSequence);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinAbort)) {
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
        release(notice.sessionId);
        return true;
    }
    if (id == wire::kParameterRequestId) {
        wire::ParameterRequestHeader header{};
        if (!wire::read_parameter_request(reader, header)) {
            return false;
        }
        const std::uint64_t mask = header.requestedMask & kParameterMaskBits;
        std::array<char, kParameterNameCapacity> names{};
        report(core::log::Level::info,
               "ev=gameplay stage=parameters result=request mask=0x%08X mode=%u names=%s",
               static_cast<unsigned>(mask),
               static_cast<unsigned>(header.modeFlag ? 1U : 0U),
               wire::parameter_names(mask, names.data(), names.size()));
        // The selected bodies are walked before the answer goes out, so nothing is answered from
        // a request that was only read as far as its header.
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
        // The peer builds no activity client until it holds the host parameter, so the answer goes
        // out even when a later body could not be located. The tail above is what is unread, not
        // the answer's own inputs. A session another endpoint holds is answered by that endpoint.
        if (!owned_elsewhere(from, header.sessionId)) {
            answer_parameters(header.sessionId, mask);
        }
        // Only a fully located request leaves the container readable behind it.
        return walk.complete;
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
        wire::PlayerAddRequest request{};
        if (!wire::read_player_add(reader, request)) {
            return false;
        }
        // The published row carries the identity group only. The profile block behind it has no
        // encoder here, and the peer's clear-flag arm accepts a row without one.
        AcquireSRWLockExclusive(&g_admittedLock);
        // The body's session, for the same reason join-complete uses its own.
        Admitted* const record = claim_owned(from, request.sessionId);
        bool published = false;
        if (record != nullptr) {
            record->hasPlayer = true;
            record->playerId = request.playerId;
            published = publish_snapshot(*record);
            // The queue is at its fullest here, right after the join promotion, so a refusal is
            // ordinary and the service slice retries it.
            record->playerPublished = published;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        // The player block and its tail are not decoded, so the body is reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=player result=%s session=0x%llX player=0x%llX seq=%u kind=%u",
               published ? "added" : "fail",
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(request.playerId),
               request.sequence,
               static_cast<unsigned>(request.kind));
        return false;
    }
    if (id == wire::kPlayerRemoveId) {
        wire::PlayerRemoveRequest request{};
        if (!wire::read_player_remove(reader, request)) {
            return false;
        }
        // The message names no player. The one to drop is the player the bound record holds.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, request.sessionId);
        bool published = false;
        if (record != nullptr && record->hasPlayer) {
            record->hasPlayer = false;
            record->playerId = 0;
            published = publish_snapshot(*record);
            record->playerPublished = published;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(core::log::Level::info,
               "ev=gameplay stage=player result=%s session=0x%llX",
               published           ? "removed"
               : record == nullptr ? "fail"
                                   : "absent",
               static_cast<unsigned long long>(request.sessionId));
        // The whole body is two fields, so the container stays readable behind it.
        return true;
    }
    if (id == wire::kPlayerPropertiesId) {
        wire::PlayerPropertiesRequest request{};
        if (!wire::read_player_properties_header(reader, request)) {
            return false;
        }
        // The sparse record behind the header is not decoded, so nothing is merged from it. A
        // merge from the header alone would reset every field the record carries.
        report(core::log::Level::info,
               "ev=gameplay stage=player result=properties session=0x%llX seq=%u kind=%u",
               static_cast<unsigned long long>(request.sessionId),
               request.sequence,
               static_cast<unsigned>(request.kind));
        return false;
    }
    // Migration and election bodies are read and recorded. This host never starts a migration and
    // never answers one, but leaving them unread would end the container at the first of them.
    return migration::consume(id, reader);
}

/** Publishes the membership snapshot that completes one peer's join. */
bool publish_membership(const state::gameplay::Endpoint& peer,
                        std::uint64_t peerJoinId,
                        std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = claim(peer, sessionId);
    bool published = false;
    if (record != nullptr) {
        // A retried join brings a new join id and drops any player the previous attempt added.
        // It also starts again at `ready`, so the previous attempt's completion does not carry.
        record->joinId = peerJoinId;
        record->sessionId = sessionId;
        record->hasPlayer = false;
        record->playerId = 0;
        record->joinComplete = false;
        record->joinPublished = false;
        record->activityHostPublished = false;
        record->playerPublished = false;
        record->lastRetry = 0;
        published = publish_snapshot(*record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    return published;
}

/** Retries any publish a full reliable queue refused. */
void service(std::uint64_t now) noexcept {
    // Outside any staged push, so the state revision it advances cannot fail a transaction guard.
    allocate_claimed_host_sessions();
    // The peer drops a stale target locally and sends no leave for it. Such a record shows up
    // only as the least recently named one over the capacity.
    std::uint64_t retired = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    std::size_t occupied = 0;
    Admitted* oldest = nullptr;
    for (Admitted& record : g_admitted) {
        if (!record.occupied) {
            continue;
        }
        ++occupied;
        if (oldest == nullptr || record.lastUse < oldest->lastUse) {
            oldest = &record;
        }
    }
    if (occupied > kPublicSessionCapacity && oldest != nullptr) {
        retired = oldest->sessionId;
        *oldest = {};
    }
    for (Admitted& record : g_admitted) {
        const bool owed =
            record.occupied
            && ((record.joinComplete && !record.joinPublished)
                || (record.joinPublished && !record.activityHostPublished)
                || (record.joinPublished && record.hasPlayer && !record.playerPublished));
        if (!owed || now - record.lastRetry < kRetryInterval) {
            continue;
        }
        record.lastRetry = now;
        if (!record.joinPublished) {
            record.joinPublished = publish_snapshot(record);
            continue;
        }
        if (!record.activityHostPublished) {
            record.activityHostPublished = publish_activity_host(record);
            continue;
        }
        // Last, so the order the join needs is unchanged. The snapshot carries the player row the
        // peer's own add asked for, and a refused one leaves the peer's player unnamed.
        record.playerPublished = publish_snapshot(record);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    // Outside the lock, in the order `release` already uses. The region's activity host stays: the
    // peer rotates back into a region it has not left, and a fresh id there is a hard error.
    if (retired != 0) {
        peer::drop(retired);
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=retired session=0x%016llX held=%zu",
               static_cast<unsigned long long>(retired),
               occupied - 1);
    }
}

/** Publishes the parameter update a joining peer needs before it will finish its join. */
bool publish_join_parameters(std::uint64_t sessionId) noexcept {
    // A joining peer finishes only once it has applied one parameter update. Any update with a
    // named parameter and no body sets that latch. This host has no values, so it releases a slot
    // the peer never filled, which leaves the peer's state alone.
    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.releasedMask = std::uint64_t{1} << kJoinLatchParameter;

    const bool sent = send_reliable(
        sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
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
                         entry.activityHostPublished,
                         entry.hasPlayer,
                         entry.playerPublished,
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
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace sunrise::server::gameplay::group
