#include <Windows.h>

#include <array>

#include "../../../middleware/gameplay/external/common_state.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../association/association_host.h"
#include "../dtls/dtls_host.h"
#include "../entity_identities.h"
#include "peer_transport_internal.h"

namespace sunrise::server::gameplay::peer {

namespace {

namespace gp = state::gameplay;
namespace wire = middleware::gameplay::peer;

/** Bit position of the payload marker inside its first byte. */
constexpr unsigned kMarkerShift = 7;

/**
 * Resolves the session a message that does not name one belongs to.
 * A link carrying more than one session leaves it unresolved rather than guessing.
 * @param peer Link the message arrived on.
 * @return The session id, or zero when the link carries none or several.
 */
[[nodiscard]] std::uint64_t sole_session_locked(const gp::PeerLink& peer) noexcept {
    std::uint64_t only = 0;
    for (const std::uint64_t held : peer.sessions) {
        if (held == 0) {
            continue;
        }
        if (only != 0) {
            return 0;
        }
        only = held;
    }
    return only;
}

} // namespace

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<gp::PeerLink, gp::kAssociationCapacity> g_peers;
std::uint64_t g_peerGeneration{};
middleware::gameplay::external::Lane0Codec g_lane0Codec{};
void (*g_lane0Outcome)(const void*, std::uint64_t, wire::AckOutcome) noexcept {};
Lane0Transport g_lane0Transport{};
middleware::gameplay::external::TypePayloadCodec g_entityCodec{};
bool (*g_entityAccepted)(const void*,
                         std::uint64_t,
                         const middleware::gameplay::external::EntityBatch&) noexcept {};
const void* g_entityAcceptedContext{};
EntityTransport g_entityTransport{};
std::uint32_t g_channelId{0};

/** Sends one payload over whichever transport the peer arrived on. */
bool send_transport(const gp::Endpoint& to, std::span<const std::byte> payload) noexcept {
    return dtls::send_payload(to, payload) || association::send_payload(to, payload);
}

/** Copies out the contributions a link is about to drop, so their owner can be told. */
std::size_t collect_displaced_locked(const gp::PeerLink& peer,
                                     DisplacedExternals& displaced) noexcept {
    std::size_t count = 0;
    for (const auto& contribution : peer.externalContributions) {
        if (contribution.occupied) {
            displaced[count++] = {contribution.groupSessionId,
                                  contribution.transmissionId,
                                  wire::AckOutcome::unresolved};
        }
    }
    return count;
}

/** Reports one batch of external outcomes to whichever channel-0 sink is installed. */
void notify_external_outcomes(const DisplacedExternals& completed, std::size_t count) noexcept {
    if (g_lane0Transport.outcome != nullptr) {
        for (std::size_t index = 0; index < count; ++index) {
            g_lane0Transport.outcome(g_lane0Transport.context,
                                     completed[index].groupSessionId,
                                     completed[index].transmissionId,
                                     completed[index].outcome);
        }
    }
    if (g_lane0Outcome != nullptr) {
        for (std::size_t index = 0; index < count; ++index) {
            g_lane0Outcome(
                g_lane0Codec.context, completed[index].transmissionId, completed[index].outcome);
        }
    }
}

/** Tells the lane-0 transport that a group session lost its channel state. */
void reset_transports(const std::uint64_t* sessions, std::size_t count) noexcept {
    if (g_lane0Transport.reset != nullptr) {
        for (std::size_t index = 0; index < count; ++index) {
            bool held = false;
            AcquireSRWLockShared(&g_lock);
            for (const auto& peer : g_peers) {
                held = held
                       || (peer.stage != gp::PeerStage::absent
                           && (carries_locked(peer, sessions[index])
                               || peer.externalGroupSessionId == sessions[index]));
            }
            ReleaseSRWLockShared(&g_lock);
            if (!held) {
                g_lane0Transport.reset(g_lane0Transport.context, sessions[index]);
            }
        }
    }
}

/** Tells the lane-0 transport that one group session lost its channel state. */
void reset_transports(std::uint64_t sessionId) noexcept {
    reset_transports(&sessionId, 1);
}

/** Source retirement follows peer-to-identity lock order and waits for publication leases. */
void invalidate_entity_identity_locked(const gp::entity_identity::Source& source) noexcept {
    if (source.groupSessionId != 0) {
        entity_identities::reset_source(source);
    }
}

/** Retires only the captured source, never a replacement sharing its group. */
void reset_entity_source(const gp::entity_identity::Source& source) noexcept {
    if (source.groupSessionId == 0) {
        return;
    }
    AcquireSRWLockShared(&g_lock);
    if (g_entityTransport.reset != nullptr) {
        g_entityTransport.reset(g_entityTransport.context, source);
    }
    ReleaseSRWLockShared(&g_lock);
}

/**
 * Copies the admitted source while its peer is locked.
 * @param peer Peer whose lifecycle is
 * being read or changed.
 * @return Exact current entity source.
 */
gp::entity_identity::Source entity_source(const gp::PeerLink& peer) noexcept {
    gp::entity_identity::Source source{};
    source.activitySessionId = peer.activityBinding.sessionId;
    source.activityRevision = peer.activityBinding.createdRevision;
    source.activityClientGeneration = peer.commonReconciler.owner_generation();
    source.groupSessionId = peer.externalGroupSessionId;
    source.peerGeneration = peer.peerGeneration;
    source.channelGeneration = peer.channelGeneration;
    source.viewGeneration = peer.viewGeneration;
    source.address = peer.endpoint.address;
    source.port = peer.endpoint.port;
    source.localPort = peer.endpoint.localPort;
    source.localConnectionSequence = peer.localConnectionSequence;
    source.remoteConnectionSequence = peer.remoteConnectionSequence;
    return source;
}

/** @return Peer for one endpoint, or null. Callers already hold the lock. */
gp::PeerLink* find_locked(const gp::Endpoint& from) noexcept {
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage != gp::PeerStage::absent && peer.endpoint == from) {
            return &peer;
        }
    }
    return nullptr;
}

/** @return True when the link carries one group session. Callers hold the lock. */
bool carries_locked(const gp::PeerLink& peer, std::uint64_t sessionId) noexcept {
    for (const std::uint64_t held : peer.sessions) {
        if (held == sessionId) {
            return true;
        }
    }
    return false;
}

/** @return Link carrying one group session, or null. Callers hold the lock. */
gp::PeerLink* find_session_locked(std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    gp::PeerLink* found = nullptr;
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage != gp::PeerStage::absent && carries_locked(peer, sessionId)) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &peer;
        }
    }
    return found;
}

/** @return Link whose authenticated external view names one group, or null. */
gp::PeerLink* find_external_group_locked(std::uint64_t groupSessionId) noexcept {
    if (groupSessionId == 0) {
        return nullptr;
    }
    gp::PeerLink* found = nullptr;
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage != gp::PeerStage::absent && peer.externalGroupSessionId == groupSessionId) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &peer;
        }
    }
    return found;
}

/** Resolves the session an out-of-band message at one endpoint belongs to. */
std::uint64_t session_for_endpoint(const gp::Endpoint& from) noexcept {
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* const peer = find_locked(from);
    const std::uint64_t only = peer == nullptr ? 0 : sole_session_locked(*peer);
    ReleaseSRWLockShared(&g_lock);
    return only;
}

/** @return A free peer slot, or null. Callers already hold the lock. */
gp::PeerLink* allocate_locked() noexcept {
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage == gp::PeerStage::absent) {
            return &peer;
        }
    }
    return nullptr;
}

/** Installs the process-lifetime channel-0 codec. */
void install_lane0_codec(const middleware::gameplay::external::Lane0Codec& codec,
                         void (*outcome)(const void*,
                                         std::uint64_t,
                                         wire::AckOutcome) noexcept) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_lane0Codec = codec;
    g_lane0Outcome = outcome;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Installs one process-lifetime, session-aware channel-0 transport. */
void install_lane0_transport(const Lane0Transport& transport) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_lane0Transport = transport;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Installs the process-lifetime session-aware channel-2 boundary. */
void install_entity_transport(const EntityTransport& transport) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_entityTransport = transport;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Retirement is serialized with packet decode before entering the codec's own lock. */
std::size_t retire_entity_baselines(
    const state::gameplay::entity_identity::Source& source,
    std::span<const state::gameplay::entity_identity::RetiredLifetime> lifetimes) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const auto retired =
        g_entityTransport.retire == nullptr
            ? 0
            : g_entityTransport.retire(g_entityTransport.context, source, lifetimes);
    ReleaseSRWLockExclusive(&g_lock);
    return retired;
}

/** Installs the process-lifetime channel-2 codec and accepted-record sink. */
void install_entity_codec(
    const middleware::gameplay::external::TypePayloadCodec& codec,
    bool (*accepted)(const void*,
                     std::uint64_t,
                     const middleware::gameplay::external::EntityBatch&) noexcept,
    const void* acceptedContext) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_entityCodec = codec;
    g_entityAccepted = accepted;
    g_entityAcceptedContext = acceptedContext;
    ReleaseSRWLockExclusive(&g_lock);
}

/** Consumes one decrypted transport payload. */
void deliver(const gp::Endpoint& from,
             std::span<const std::byte> payload,
             std::uint64_t now) noexcept {
    if (payload.empty()) {
        return;
    }
    if ((std::to_integer<unsigned>(payload[0]) >> kMarkerShift) != 0) {
        consume_container(from, payload, now);
        return;
    }
    consume_established(from, payload, now);
}

/** Queues one reliable message for a peer. */
bool enqueue_reliable(std::uint64_t sessionId,
                      std::uint8_t id,
                      std::uint32_t declaredSize,
                      std::span<const std::byte> body,
                      std::size_t bodyBits) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* peer = find_session_locked(sessionId);
    const bool queued = peer != nullptr && peer->establishQueued
                        && wire::enqueue_message(peer->outbound, id, declaredSize, body, bodyBits);
    if (queued) {
        // The next service slice carries it, so the acknowledgement path also flushes sends.
        peer->acknowledgementOwed = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return queued;
}

/** Queues one sessionless reliable message on the exact peer endpoint. */
bool enqueue_reliable(const gp::Endpoint& endpoint,
                      std::uint8_t id,
                      std::uint32_t declaredSize,
                      std::span<const std::byte> body,
                      std::size_t bodyBits) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* peer = find_locked(endpoint);
    const bool queued = peer != nullptr && peer->establishQueued
                        && wire::enqueue_message(peer->outbound, id, declaredSize, body, bodyBits);
    if (queued) {
        peer->acknowledgementOwed = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return queued;
}

/** Reports the NetAddr one peer sent in its own connect request. */
bool remote_address(std::uint64_t sessionId,
                    std::array<std::byte, gp::kNetAddrBlobSize>& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr && peer->remoteAddressPresent;
    if (present) {
        output = peer->remoteAddress;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

bool remote_address(const gp::Endpoint& endpoint,
                    std::uint64_t sessionId,
                    std::array<std::byte, gp::kNetAddrBlobSize>& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto* peer = find_locked(endpoint);
    const bool present =
        peer != nullptr && carries_locked(*peer, sessionId) && peer->remoteAddressPresent;
    if (present) {
        output = peer->remoteAddress;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Binds one peer's view signature. */
void bind_view(const gp::Endpoint& from, const gp::ViewSignature& signature) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    // Keyed by endpoint, not by session. The view body carries no session id, and a link holding
    // both a current and a target region resolves no sole session to key it by.
    gp::PeerLink* peer = find_locked(from);
    if (peer != nullptr) {
        peer->view = signature;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Retains one inbound view stage and exposes its pending response. */
ViewStageResult receive_view_stage(const gp::Endpoint& from,
                                   const middleware::gameplay::group::ViewEstablishment& input,
                                   middleware::gameplay::group::ViewEstablishment& response,
                                   std::uint64_t& generation) noexcept {
    response = {};
    generation = 0;
    DisplacedExternals displaced{};
    std::size_t displacedCount = 0;
    std::uint64_t resetSessionId = 0;
    gp::entity_identity::Source resetSource{};
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* const peer = find_locked(from);
    if (peer == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return ViewStageResult::noPeer;
    }
    auto& receptor = peer->viewReceptor;
    bool accepted = false;
    if (receptor.phase() == gp::external::view_receptor::Phase::closed) {
        resetSource = entity_source(*peer);
        invalidate_entity_identity_locked(resetSource);
        ++peer->viewGeneration;
        if (peer->viewGeneration == 0) {
            ++peer->viewGeneration;
        }
        accepted = receptor.open_from_stage_one(input, peer->viewGeneration);
        if (accepted) {
            resetSessionId = peer->externalGroupSessionId;
            displacedCount = collect_displaced_locked(*peer, displaced);
            peer->view = {};
            peer->commonReconciler.reset();
            peer->commonCommitted = false;
            peer->externalGroupSessionId = 0;
            peer->activityBinding = {};
            peer->externalShadow = {};
            peer->externalContributions = {};
        }
    } else {
        const auto result = receptor.receive(input);
        accepted = result == gp::external::view_receptor::ReceiveResult::accepted
                   || result == gp::external::view_receptor::ReceiveResult::responsePending;
    }
    if (accepted && input.kind == 2) {
        peer->view.token = input.sessionToken;
        peer->view.kind = input.kind;
        peer->view.optionalValue = input.optionalValue;
        peer->view.hasOptionalValue = input.hasOptionalValue;
        peer->view.list = input.list;
        peer->view.listCount = input.listCount;
        peer->view.hasList = input.hasList;
    }
    accepted = accepted && receptor.pending(response);
    if (accepted && response.kind == 5
        && peer->commonReconciler.phase() != gp::external::common_reconciler::Phase::ready) {
        accepted = false;
        response = {};
    }
    if (accepted) {
        generation = receptor.generation();
    }
    ReleaseSRWLockExclusive(&g_lock);
    notify_external_outcomes(displaced, displacedCount);
    if (resetSessionId != 0) {
        reset_transports(resetSessionId);
    }
    reset_entity_source(resetSource);
    return accepted ? ViewStageResult::accepted : ViewStageResult::refused;
}

/** Commits one response and opens the flat accepted-view gate at stage 5. */
bool commit_view_response(const gp::Endpoint& from, std::uint64_t generation) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* const peer = find_locked(from);
    middleware::gameplay::group::ViewEstablishment pending{};
    bool committed =
        peer != nullptr && generation != 0 && peer->viewReceptor.generation() == generation
        && peer->viewReceptor.pending(pending)
        && (pending.kind != 5
            || peer->commonReconciler.phase() == gp::external::common_reconciler::Phase::ready)
        && peer->viewReceptor.commit_pending();
    if (committed && peer->viewReceptor.phase() == gp::external::view_receptor::Phase::accepted) {
        peer->view.bound = true;
        peer->view.kind = peer->viewReceptor.local_stage();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return committed;
}

/** Opens common reconciliation for one exact ActivityClient generation. */
bool open_external_common(
    std::uint64_t groupSessionId,
    const state::activity::SessionBinding& activity,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
    std::uint64_t activityClientGeneration,
    std::uint8_t replicationEpoch) noexcept {
    gp::Endpoint endpoint{};
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* const peer = find_session_locked(groupSessionId);
    const bool present = peer != nullptr;
    if (present) {
        endpoint = peer->endpoint;
    }
    ReleaseSRWLockShared(&g_lock);
    return present
           && open_external_common(endpoint,
                                   groupSessionId,
                                   activity,
                                   patchEpoch,
                                   activityClientGeneration,
                                   replicationEpoch);
}

/** Opens common reconciliation before the group join binds its session to the endpoint. */
bool open_external_common(
    const gp::Endpoint& endpoint,
    std::uint64_t groupSessionId,
    const state::activity::SessionBinding& activity,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
    std::uint64_t activityClientGeneration,
    std::uint8_t replicationEpoch) noexcept {
    if (groupSessionId == 0) {
        return false;
    }
    DisplacedExternals displaced{};
    std::size_t displacedCount = 0;
    std::uint64_t resetSessionId = 0;
    gp::entity_identity::Source resetSource{};
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* const peer = find_locked(endpoint);
    const auto previousSource =
        peer != nullptr ? entity_source(*peer) : gp::entity_identity::Source{};
    const bool same =
        peer != nullptr && peer->externalGroupSessionId == groupSessionId
        && peer->activityBinding.sessionId == activity.sessionId
        && peer->activityBinding.createdRevision == activity.createdRevision
        && peer->commonReconciler.owner_generation() == activityClientGeneration
        && peer->commonReconciler.phase() != gp::external::common_reconciler::Phase::closed
        && peer->commonReconciler.phase() != gp::external::common_reconciler::Phase::failed;
    auto nextReconciler =
        peer != nullptr ? peer->commonReconciler : gp::external::common_reconciler::Reconciler{};
    const bool opened =
        same
        || (peer != nullptr && peer->viewGeneration != 0
            && nextReconciler.open_known(
                activity.sessionId, patchEpoch, activityClientGeneration, replicationEpoch));
    if (opened) {
        auto nextSource = previousSource;
        nextSource.activitySessionId = activity.sessionId;
        nextSource.activityRevision = activity.createdRevision;
        nextSource.activityClientGeneration = nextReconciler.owner_generation();
        nextSource.groupSessionId = groupSessionId;
        if (nextSource != previousSource) {
            resetSource = previousSource;
            invalidate_entity_identity_locked(resetSource);
        }
        peer->commonReconciler = nextReconciler;
        peer->activityBinding = activity;
        if (!same) {
            resetSessionId = peer->externalGroupSessionId;
            displacedCount = collect_displaced_locked(*peer, displaced);
            peer->externalGroupSessionId = groupSessionId;
            peer->commonCommitted = false;
            peer->externalContributions = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    notify_external_outcomes(displaced, displacedCount);
    if (resetSessionId != 0) {
        reset_transports(resetSessionId);
    }
    reset_entity_source(resetSource);
    return opened;
}

/**
 * Advances matching views without replacing their entity source or baseline store.
 * @param activity Exact admitted activity binding.
 * @param activityClientGeneration Owner of the committed host operation.
 * @param expectedEpoch Previously authored epoch.
 * @param nextEpoch Epoch carried by the committed operation.
 * @return Number of views whose epoch advanced.
 */
std::size_t commit_replication_epoch(const state::activity::SessionBinding& activity,
                                     std::uint64_t activityClientGeneration,
                                     std::uint8_t expectedEpoch,
                                     std::uint8_t nextEpoch) noexcept {
    if (activity.sessionId == 0 || activity.createdRevision == 0 || activityClientGeneration == 0) {
        return 0;
    }
    std::size_t advanced = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage == gp::PeerStage::absent || peer.externalGroupSessionId == 0
            || peer.activityBinding.sessionId != activity.sessionId
            || peer.activityBinding.createdRevision != activity.createdRevision
            || peer.commonReconciler.owner_generation() != activityClientGeneration
            || !peer.commonReconciler.advance_host_epoch(expectedEpoch, nextEpoch)) {
            continue;
        }
        const auto source = entity_source(peer);
        const auto domain = peer.commonReconciler.allocation_domain();
        if (g_entityTransport.advanceEpoch != nullptr) {
            g_entityTransport.advanceEpoch(
                g_entityTransport.context, source, expectedEpoch, nextEpoch, domain);
        }
        static_cast<void>(
            entity_identities::advance_epoch(source, expectedEpoch, nextEpoch, domain));
        peer.commonCommitted = false;
        for (auto& contribution : peer.externalContributions) {
            contribution.commonPresent = false;
        }
        peer.acknowledgementOwed = true;
        ++advanced;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return advanced;
}

/** Reports whether every native outbound gate is open. */
bool external_outbound_eligible(std::uint64_t groupSessionId) noexcept {
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* const peer = find_session_locked(groupSessionId);
    middleware::gameplay::external::CommonState common{};
    const auto viewPhase =
        peer == nullptr ? gp::external::view_receptor::Phase::closed : peer->viewReceptor.phase();
    const bool eligible = peer != nullptr && peer->stage == gp::PeerStage::connected
                          && peer->externalGroupSessionId == groupSessionId
                          && (viewPhase == gp::external::view_receptor::Phase::provisional
                              || viewPhase == gp::external::view_receptor::Phase::accepted)
                          && peer->commonReconciler.outbound_common(common)
                          && (g_lane0Transport.write != nullptr || g_lane0Codec.write != nullptr);
    ReleaseSRWLockShared(&g_lock);
    return eligible;
}

/** Reports whether the link carrying one session holds a bound view and is established. */
bool view_bound(std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* peer = find_session_locked(sessionId);
    // A bound body alone is not readiness. The link also has to be past its connect exchange, or
    // the view belongs to a channel the peer has already rebuilt.
    const bool ready =
        peer != nullptr && peer->view.bound && peer->stage == gp::PeerStage::connected;
    ReleaseSRWLockShared(&g_lock);
    return ready;
}

/** Reports how far the link carrying one group session has got. */
bool link_stage(std::uint64_t sessionId, gp::PeerStage& stage) noexcept {
    stage = gp::PeerStage::absent;
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* peer = find_session_locked(sessionId);
    const bool present = peer != nullptr;
    if (present) {
        stage = peer->stage;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

/** Copies the connect sequences of the link carrying one joined or external group. */
bool link_identity(std::uint64_t sessionId, LinkIdentity& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const gp::PeerLink* peer = find_session_locked(sessionId);
    if (peer == nullptr) {
        peer = find_external_group_locked(sessionId);
    }
    const bool present = peer != nullptr;
    if (present) {
        output.localConnectionSequence = peer->localConnectionSequence;
        output.remoteConnectionSequence = peer->remoteConnectionSequence;
        output.viewGeneration = peer->viewGeneration;
    }
    ReleaseSRWLockShared(&g_lock);
    return present;
}

namespace {
void drop_link(const gp::Endpoint* endpoint, std::uint64_t sessionId) noexcept {
    DisplacedExternals displaced{};
    std::size_t displacedCount = 0;
    gp::entity_identity::Source resetSource{};
    AcquireSRWLockExclusive(&g_lock);
    gp::PeerLink* const peer =
        endpoint != nullptr ? find_locked(*endpoint) : find_session_locked(sessionId);
    if (peer != nullptr && carries_locked(*peer, sessionId)) {
        if (peer->externalGroupSessionId == sessionId) {
            resetSource = entity_source(*peer);
            invalidate_entity_identity_locked(resetSource);
        }
        // The channel outlives the session. A leave names one region, and the client keeps playing
        // the other over the same channel.
        for (std::uint64_t& slot : peer->sessions) {
            if (slot == sessionId) {
                slot = 0;
            }
        }
        if (peer->externalGroupSessionId == sessionId) {
            resetSource = entity_source(*peer);
            displacedCount = collect_displaced_locked(*peer, displaced);
            peer->externalGroupSessionId = 0;
            peer->activityBinding = {};
            peer->commonReconciler = {};
            peer->commonCommitted = false;
            peer->externalContributions = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    notify_external_outcomes(displaced, displacedCount);
    reset_transports(sessionId);
    reset_entity_source(resetSource);
}
} // namespace

/** Drops one unambiguously owned group, leaving other groups on its link alone. */
void drop(std::uint64_t sessionId) noexcept {
    drop_link(nullptr, sessionId);
}

void drop(const gp::Endpoint& endpoint, std::uint64_t sessionId) noexcept {
    drop_link(&endpoint, sessionId);
}

/** Drops every link at one endpoint, which is what a connect-closed names. */
void drop_endpoint(const gp::Endpoint& endpoint) noexcept {
    std::array<std::uint64_t, gp::kAssociationCapacity * gp::kSessionsPerLink> sessions{};
    std::size_t sessionCount = 0;
    std::array<gp::entity_identity::Source, gp::kAssociationCapacity> sources{};
    std::size_t sourceCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (gp::PeerLink& peer : g_peers) {
        if (peer.stage != gp::PeerStage::absent && peer.endpoint == endpoint) {
            sources[sourceCount++] = entity_source(peer);
            invalidate_entity_identity_locked(sources[sourceCount - 1]);
            for (const std::uint64_t sessionId : peer.sessions) {
                if (sessionId != 0) {
                    sessions[sessionCount++] = sessionId;
                }
            }
            peer = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    reset_transports(sessions.data(), sessionCount);
    for (std::size_t index = 0; index < sourceCount; ++index) {
        reset_entity_source(sources[index]);
    }
}

/** Drops every peer. */
void reset() noexcept {
    std::array<std::uint64_t, gp::kAssociationCapacity * gp::kSessionsPerLink> sessions{};
    std::size_t sessionCount = 0;
    std::array<gp::entity_identity::Source, gp::kAssociationCapacity> sources{};
    std::size_t sourceCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    reset_packet_assemblies();
    for (gp::PeerLink& peer : g_peers) {
        sources[sourceCount++] = entity_source(peer);
        invalidate_entity_identity_locked(sources[sourceCount - 1]);
        for (const std::uint64_t sessionId : peer.sessions) {
            if (sessionId != 0) {
                sessions[sessionCount++] = sessionId;
            }
        }
        peer = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
    reset_transports(sessions.data(), sessionCount);
    for (std::size_t index = 0; index < sourceCount; ++index) {
        reset_entity_source(sources[index]);
    }
}

} // namespace sunrise::server::gameplay::peer
