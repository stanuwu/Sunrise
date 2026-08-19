/**
 * Host-migration and election bodies. Each one is read into typed fields and nothing more: this
 * host keeps a stable host, and an answer built from a half-read election can leave two peers each
 * believing they carry the group.
 */

#include "migration_messages.h"

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** The successor member index is six bits. */
constexpr std::uint8_t kSuccessorIndexWidth = 6;
/** Handoff progress is seven bits, so the wire can declare more than a percentage. */
constexpr std::uint8_t kProgressWidth = 7;
/** The transition token is a raw 32-bit field. */
constexpr std::size_t kTransitionTokenBytes = 4;
/** Key material the reestablishment carries, in bytes. It is skipped, never kept. */
constexpr std::size_t kKeyMaterialBytes = 16;
/** The online session id the reestablishment carries, in bytes. It is skipped, never kept. */
constexpr std::size_t kOnlineSessionBytes = 18;
/** Every optional field is introduced by one presence bit. */
constexpr std::uint8_t kPresenceWidth = 1;
/** The candidate count is six bits, so the wire can declare more than the bound allows. */
constexpr std::uint8_t kCandidateCountWidth = 6;
/** The refuse code is four bits. */
constexpr std::uint8_t kRefuseCodeWidth = 4;
/** Raw byte fields are read as whole bytes. */
constexpr std::uint8_t kBitsPerByte = 8;

} // namespace

/** Reads a host-handoff or peer-handoff body. */
bool read_host_handoff(bits::Reader& reader, HostHandoff& output) noexcept {
    HostHandoff candidate{};
    std::uint64_t index = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw(reader, candidate.successorAddress)
        || !reader.read(kSuccessorIndexWidth, index)) {
        return false;
    }
    candidate.successorIndex = static_cast<std::uint8_t>(index);
    output = candidate;
    return true;
}

/** Reads a host-transition body. */
bool read_host_transition(bits::Reader& reader, HostTransition& output) noexcept {
    HostTransition candidate{};
    std::uint64_t progress = 0;
    std::array<std::byte, kTransitionTokenBytes> token{};
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kProgressWidth, progress)
        || !bits::read_raw(reader, token)) {
        return false;
    }
    if (progress > kMaximumHandoffProgress) {
        return false;
    }
    candidate.progress = static_cast<std::uint8_t>(progress);
    for (std::size_t index = 0; index < token.size(); ++index) {
        candidate.transitionToken |= std::to_integer<std::uint32_t>(token[index])
                                     << (static_cast<unsigned>(index) * kBitsPerByte);
    }
    output = candidate;
    return true;
}

/** Reads a host-reestablish body. */
bool read_host_reestablish(bits::Reader& reader, HostReestablish& output) noexcept {
    HostReestablish candidate{};
    // The key material and online session id belong to the peer's own session. They are consumed
    // so the body is framed and are never copied out.
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw_u64(reader, candidate.machineId)
        || !bits::read_raw(reader, candidate.address) || !bits::skip_raw(reader, kKeyMaterialBytes)
        || !bits::skip_raw(reader, kOnlineSessionBytes)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Reads a session-only migration body. */
bool read_migration_session(bits::Reader& reader, std::uint64_t& sessionId) noexcept {
    sessionId = 0;
    return bits::read_raw_u64(reader, sessionId);
}

/** Reads a host-decline body. */
bool read_host_decline(bits::Reader& reader, HostDecline& output) noexcept {
    HostDecline candidate{};
    std::uint64_t present = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kPresenceWidth, present)) {
        return false;
    }
    candidate.hasDeclineData = present != 0;
    if (!candidate.hasDeclineData) {
        output = candidate;
        return true;
    }
    std::uint64_t flag = 0;
    std::uint64_t hasAddress = 0;
    if (!reader.read(kPresenceWidth, flag) || !reader.read(kPresenceWidth, hasAddress)) {
        return false;
    }
    candidate.declineFlag = flag != 0;
    candidate.hasAddress = hasAddress != 0;
    if (candidate.hasAddress && !bits::read_raw(reader, candidate.address)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Reads an election as far as its candidate addresses. */
bool read_election(bits::Reader& reader, Election& output) noexcept {
    Election candidate{};
    std::uint64_t count = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw(reader, candidate.previousHost)
        || !reader.read(kCandidateCountWidth, count)) {
        return false;
    }
    if (count > kMaximumCandidates) {
        return false;
    }
    candidate.candidateCount = static_cast<std::uint8_t>(count);
    for (std::uint8_t index = 0; index < candidate.candidateCount; ++index) {
        if (!bits::skip_raw(reader, descriptor::kNetAddrSize)) {
            return false;
        }
    }
    // The per-candidate value width is unrecovered, so the three bitsets behind it cannot be
    // located and the rest of the body stays one bounded region.
    candidate.tailBits = static_cast<std::uint32_t>(reader.remaining_bits());
    output = candidate;
    return true;
}

/** Reads an election refusal. */
bool read_election_refuse(bits::Reader& reader, ElectionRefuse& output) noexcept {
    ElectionRefuse candidate{};
    std::uint64_t code = 0;
    std::uint64_t present = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kRefuseCodeWidth, code)
        || !reader.read(kPresenceWidth, present)) {
        return false;
    }
    if (code < kMinimumRefuseCode || code > kMaximumRefuseCode) {
        return false;
    }
    candidate.refuseCode = static_cast<std::uint8_t>(code);
    candidate.hasCandidateAddress = present != 0;
    if (candidate.hasCandidateAddress && !bits::read_raw(reader, candidate.candidateAddress)) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::gameplay::group
