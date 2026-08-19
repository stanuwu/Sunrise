#include "dtls_host.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <type_traits>

#include "../../../middleware/crypto/ecc_p224.h"
#include "../../../middleware/crypto/random_bytes.h"
#include "../../../middleware/gameplay/dtls/association_keys.h"
#include "../../../middleware/gameplay/dtls/dtls_messages.h"
#include "../../../middleware/gameplay/dtls/record.h"
#include "../../../middleware/gameplay/dtls/replay_high_water.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../peer/peer_transport.h"

namespace sunrise::server::gameplay::dtls {

namespace {

namespace wire = middleware::gameplay::dtls;

/** Concurrent associations. One citizen join needs one, and a retry reuses its endpoint. */
constexpr std::size_t kAssociationCapacity = 8;
/** An association that never reaches the key exchange is dropped after this many milliseconds. */
constexpr std::uint64_t kHandshakeTimeout = 30000;

/** How far one association has progressed. */
enum class Stage : std::uint8_t { absent, cookieWait, established };

/** One peer's handshake state. */
struct Association {
    state::gameplay::Endpoint endpoint{};
    Stage stage{Stage::absent};
    /** Tag the peer chose. Every packet this host sends must name it. */
    std::uint16_t requesterTag{};
    /** Tag this host chose. The peer names it once it has the init ack. */
    std::uint16_t responderTag{};
    /** Id the peer routes on. Every packet must repeat it or the peer never sees it. */
    wire::SecurityId securityId{};
    /** The init ack exactly as it left, which the cookie echo has to return unaltered. */
    std::array<std::byte, wire::kInitAckSize> issued{};
    /** Keys and tag every record of this association uses. */
    middleware::gameplay::dtls::RecordContext record{};
    /** Authenticated record sequences already admitted on this association. */
    middleware::gameplay::dtls::ReplayHighWater receiveHighWater{};
    /** False until one received record names the digest that authenticates it. */
    bool authKnown{};
    /** Sequence the next sent record carries. */
    std::uint32_t sendSequence{1};
    /** Tick the handshake last advanced. */
    std::uint64_t touched{};
    /** Order this association reached `established`. */
    std::uint64_t opened{};
    /** Order one record last arrived on it. The peer reads where it writes. */
    std::uint64_t heard{};
};

/** Stamps `opened` and `heard`. They only have to order associations, so neither is a clock. */
std::uint64_t g_openClock{0};

/** Join key the descriptor advertises. The derivation mixes it in. */
constexpr std::array<std::byte, middleware::gameplay::dtls::kSecurityKeySize> kJoinKey{};

/** Record arrivals reported per run. Enough to show the framing without a flood filling the log. */
constexpr unsigned kMaxRecordReports = 24;
/** Message type of a data record. */
constexpr std::uint8_t kRecordType = 6;

std::atomic<unsigned> g_recordReported{0};

std::array<Association, kAssociationCapacity> g_associations{};

/** @return True when both endpoints name the same address and port. */
[[nodiscard]] bool same_endpoint(const state::gameplay::Endpoint& left,
                                 const state::gameplay::Endpoint& right) noexcept {
    return left.address == right.address && left.port == right.port;
}

/**
 * Finds the association one init belongs to, or takes a slot for it.
 * The peer holds several at once, so an established one is displaced only as a last resort.
 * @param from Source endpoint.
 * @param securityId Security id the init named.
 * @param now Monotonic tick count in milliseconds.
 * @return The slot, or null when every slot is a live association for another security id.
 */
[[nodiscard]] Association* acquire(const state::gameplay::Endpoint& from,
                                   const wire::SecurityId& securityId,
                                   std::uint64_t now) noexcept {
    Association* free = nullptr;
    Association* oldest = nullptr;
    for (Association& association : g_associations) {
        if (association.stage != Stage::absent && same_endpoint(association.endpoint, from)
            && association.securityId == securityId) {
            // The peer restarts its own handshake on every retry, so its own id reuses the slot.
            return &association;
        }
        if (free == nullptr
            && (association.stage == Stage::absent
                || (association.stage == Stage::cookieWait
                    && now - association.touched > kHandshakeTimeout))) {
            free = &association;
        }
        if (association.stage == Stage::established
            && (oldest == nullptr || association.touched < oldest->touched)) {
            oldest = &association;
        }
    }
    // Nothing free, so the least recently used established association goes.
    return free != nullptr ? free : oldest;
}

/**
 * Finds the association one received record is addressed to.
 * @param from Source endpoint.
 * @param tag Tag the record names, which is the one this host chose for that association.
 * @return The established association, or null.
 */
[[nodiscard]] Association* find_addressed(const state::gameplay::Endpoint& from,
                                          std::uint16_t tag) noexcept {
    for (Association& association : g_associations) {
        if (association.stage == Stage::established && same_endpoint(association.endpoint, from)
            && association.responderTag == tag) {
            return &association;
        }
    }
    return nullptr;
}

/**
 * Finds the association this host sends on for one endpoint.
 * The peer reads where it writes, so a reply goes on the association its records last arrived on.
 * @param to Peer endpoint.
 * @return The established association the peer last used, or null.
 */
[[nodiscard]] Association* find_sending(const state::gameplay::Endpoint& to) noexcept {
    Association* chosen = nullptr;
    for (Association& association : g_associations) {
        if (association.stage != Stage::established || !same_endpoint(association.endpoint, to)) {
            continue;
        }
        // Both stamps start at zero, so a fresh association wins only until a record arrives.
        if (chosen == nullptr || association.heard > chosen->heard
            || (association.heard == chosen->heard && association.opened > chosen->opened)) {
            chosen = &association;
        }
    }
    return chosen;
}

/** @return The association for one endpoint whose handshake is still open, or null. */
[[nodiscard]] Association* find_handshake(const state::gameplay::Endpoint& from,
                                          const wire::SecurityId& securityId) noexcept {
    for (Association& association : g_associations) {
        if (association.stage != Stage::absent && same_endpoint(association.endpoint, from)
            && association.securityId == securityId) {
            return &association;
        }
    }
    return nullptr;
}

/**
 * Draws one nonzero 16-bit tag.
 * @param output Receives the tag only on success.
 * @return True when Windows produced the bytes.
 */
[[nodiscard]] bool generate_tag(std::uint16_t& output) noexcept {
    /** Bits in one byte. */
    constexpr unsigned kByteBits = 8;
    std::array<std::byte, sizeof(std::uint16_t)> bytes{};
    if (!middleware::crypto::random::fill(bytes)) {
        return false;
    }
    const auto value =
        static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[0])
                                   | (std::to_integer<std::uint16_t>(bytes[1]) << kByteBits));
    // A zero tag reads as "no association" on the peer's side.
    output = value == 0 ? 1 : value;
    return true;
}

/**
 * Answers one init with an init ack.
 * @param from Source endpoint.
 * @param datagram Received bytes.
 * @param now Monotonic tick count in milliseconds.
 */
void on_init(const state::gameplay::Endpoint& from,
             std::span<const std::byte> datagram,
             std::uint64_t now) noexcept {
    wire::Init init{};
    if (!wire::read_init(datagram, init)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=dtls result=drop reason=init_decode bytes=%zu",
               datagram.size());
        return;
    }
    Association* association = acquire(from, init.securityId, now);
    std::uint16_t responderTag = 0;
    if (association == nullptr || !generate_tag(responderTag)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=dtls result=drop reason=%s",
               association == nullptr ? "no_slot" : "no_random");
        return;
    }

    wire::InitAck initAck{};
    initAck.requesterTag = init.initTag;
    initAck.responderTag = responderTag;
    initAck.address = from.address;
    initAck.port = from.port;
    initAck.timestamp = static_cast<std::uint32_t>(now);
    initAck.securityId = init.securityId;
    // The cookie is only ever compared with the copy kept here, so a random value serves.
    if (!middleware::crypto::random::fill(initAck.cookie)) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=drop reason=no_random");
        return;
    }

    std::array<std::byte, wire::kInitAckSize> encoded{};
    wire::write_init_ack(initAck, encoded);

    *association = {};
    association->endpoint = from;
    association->stage = Stage::cookieWait;
    association->requesterTag = init.initTag;
    association->responderTag = responderTag;
    association->securityId = init.securityId;
    association->issued = encoded;
    association->touched = now;

    const bool sent = endpoint::send_to(from, encoded);
    report(core::log::Level::info,
           "ev=gameplay stage=dtls result=%s step=init_ack peer_tag=0x%04X local_tag=0x%04X",
           sent ? "ok" : "send_failed",
           static_cast<unsigned>(init.initTag),
           static_cast<unsigned>(responderTag));
}

/**
 * Answers one cookie echo.
 * @param from Source endpoint.
 * @param datagram Received bytes.
 * @param now Monotonic tick count in milliseconds.
 */
void on_cookie_echo(const state::gameplay::Endpoint& from,
                    std::span<const std::byte> datagram,
                    std::uint64_t now) noexcept {
    wire::CookieEcho echo{};
    if (!wire::read_cookie_echo(datagram, echo)) {
        report(core::log::Level::warn,
               "ev=gameplay stage=dtls result=drop reason=cookie_decode bytes=%zu",
               datagram.size());
        return;
    }
    Association* association = find_handshake(from, echo.securityId);
    if (association == nullptr) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=drop reason=no_association");
        return;
    }
    // The peer returns the init ack whole, so comparing it covers the cookie and its bound fields.
    if (echo.echoedInitAck != association->issued) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=drop reason=cookie_mismatch");
        return;
    }

    association->touched = now;

    middleware::crypto::ecc::Agreement agreement{};
    if (!middleware::crypto::ecc::agree(echo.publicKey, agreement)) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=drop reason=key_agreement");
        return;
    }
    const bool derived = middleware::gameplay::dtls::derive(
        agreement.sharedSecret, kJoinKey, association->record.keys);
    SecureZeroMemory(agreement.sharedSecret.data(), agreement.sharedSecret.size());
    if (!derived) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=drop reason=key_derivation");
        return;
    }
    // Every record names the tag the peer chose for itself.
    association->record.sendTag = association->requesterTag;

    wire::CookieAck cookieAck{};
    cookieAck.requesterTag = association->requesterTag;
    cookieAck.securityId = association->securityId;
    cookieAck.publicKey = agreement.publicKey;

    std::array<std::byte, wire::kCookieAckSize> encoded{};
    wire::write_cookie_ack(cookieAck, encoded);
    const bool sent = endpoint::send_to(from, encoded);
    association->stage = sent ? Stage::established : Stage::cookieWait;
    if (sent) {
        ++g_openClock;
        association->opened = g_openClock;
    }
    report(core::log::Level::info,
           "ev=gameplay stage=dtls result=%s step=cookie_ack peer_tag=0x%04X",
           sent ? "ok" : "send_failed",
           static_cast<unsigned>(association->requesterTag));
}

/**
 * Opens one received record and hands its payload to the peer transport.
 * @param from Source endpoint.
 * @param datagram Received bytes.
 * @param now Monotonic tick count in milliseconds.
 */
void on_record(const state::gameplay::Endpoint& from,
               std::span<const std::byte> datagram,
               std::uint64_t now) noexcept {
    // One endpoint carries one association per security id, so the record's own tag picks it.
    std::uint16_t addressed = 0;
    if (!middleware::gameplay::dtls::read_record_tag(datagram, addressed)) {
        return;
    }
    Association* association = find_addressed(from, addressed);
    if (association == nullptr) {
        return;
    }
    // The peer's digest choice is not announced, so the first record it sends names it.
    if (!association->authKnown) {
        if (!middleware::gameplay::dtls::identify_auth(
                association->record.keys, datagram, association->record.authAlgorithm)) {
            if (g_recordReported.fetch_add(1, std::memory_order_relaxed) < kMaxRecordReports) {
                report(core::log::Level::warn,
                       "ev=gameplay stage=dtls result=drop reason=auth_unknown bytes=%zu",
                       datagram.size());
            }
            return;
        }
        association->authKnown = true;
        report(core::log::Level::info,
               "ev=gameplay stage=dtls result=ok step=auth digest=%u",
               static_cast<unsigned>(association->record.authAlgorithm));
    }

    std::array<std::byte, middleware::gameplay::dtls::kRecordCapacity> payload{};
    std::size_t size = 0;
    std::uint32_t sequence = 0;
    if (!middleware::gameplay::dtls::open(association->record, datagram, payload, size, sequence)) {
        if (g_recordReported.fetch_add(1, std::memory_order_relaxed) < kMaxRecordReports) {
            report(core::log::Level::warn,
                   "ev=gameplay stage=dtls result=drop reason=record bytes=%zu",
                   datagram.size());
        }
        return;
    }
    const wire::ReplayDecision replay = wire::update(association->receiveHighWater, sequence);
    if (replay != wire::ReplayDecision::accepted) {
        if (g_recordReported.fetch_add(1, std::memory_order_relaxed) < kMaxRecordReports) {
            report(core::log::Level::warn,
                   "ev=gameplay stage=dtls result=drop reason=%s seq=%u",
                   replay == wire::ReplayDecision::duplicate ? "replay_duplicate" : "replay_old",
                   sequence);
        }
        return;
    }
    association->touched = now;
    ++g_openClock;
    association->heard = g_openClock;
    if (g_recordReported.fetch_add(1, std::memory_order_relaxed) < kMaxRecordReports) {
        report(core::log::Level::info,
               "ev=gameplay stage=dtls result=ok step=record seq=%u bytes=%zu",
               sequence,
               size);
    }
    peer::deliver(from, {payload.data(), size}, now);
}

} // namespace

/** Seals one transport payload and sends it to an established association. */
bool send_payload(const state::gameplay::Endpoint& to,
                  std::span<const std::byte> payload) noexcept {
    Association* association = find_sending(to);
    if (association == nullptr) {
        return false;
    }
    std::array<std::byte, middleware::gameplay::dtls::kRecordCapacity> datagram{};
    std::size_t size = 0;
    if (!middleware::gameplay::dtls::seal(
            association->record, association->sendSequence, payload, datagram, size)) {
        report(core::log::Level::warn, "ev=gameplay stage=dtls result=fail step=seal");
        return false;
    }
    ++association->sendSequence;
    return endpoint::send_to(to, {datagram.data(), size});
}

/** Answers one association handshake datagram. */
bool route(const state::gameplay::Endpoint& from,
           std::span<const std::byte> datagram,
           std::uint64_t now) noexcept {
    std::uint8_t type = 0;
    if (!wire::read_type(datagram, type)) {
        return false;
    }
    if (type == static_cast<std::uint8_t>(wire::Type::init)) {
        on_init(from, datagram, now);
        return true;
    }
    if (type == static_cast<std::uint8_t>(wire::Type::cookieEcho)) {
        on_cookie_echo(from, datagram, now);
        return true;
    }
    if (type == kRecordType) {
        on_record(from, datagram, now);
        return true;
    }
    return false;
}

/** Drops every association and clears its key material. */
void reset() noexcept {
    // An assignment can be elided, and the table holds derived keys.
    static_assert(std::is_trivially_copyable_v<Association>, "the table is erased as raw bytes");
    SecureZeroMemory(g_associations.data(), sizeof(g_associations));
    g_openClock = 0;
}

} // namespace sunrise::server::gameplay::dtls
