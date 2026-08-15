/**
 * Msg 19 targets index a 7,763-record table that the Client reads without a bound check, so a bad
 * index is a crash and not a decode error. Rows 795, 4690 and 5375 hold type code -1 and are the
 * same risk. This validator rejects both before anything acts on the body.
 * A compressed target selector ends decoding: its wire length is not recoverable from this build,
 * so the fields behind one cannot be located.
 */

#include <algorithm>
#include <climits>

#include "../../encoding/bit_reader.h"
#include "incident.h"

namespace sunrise::middleware::bap::activity_message::incident {
namespace {

/** @return True when one target index is safe to hand to the Client's table lookup. */
[[nodiscard]] bool target_allowed(std::uint32_t target, Verdict& verdict) noexcept {
    if (target > kTargetMaximum) {
        verdict = Verdict::targetOutOfRange;
        return false;
    }
    if (std::find(kPoisonTargets.begin(), kPoisonTargets.end(), target) != kPoisonTargets.end()) {
        verdict = Verdict::targetPoisoned;
        return false;
    }
    return true;
}

} // namespace

/** @return A short stable name for one verdict, for the log line. */
const char* verdict_name(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::accepted:
        return "accepted";
    case Verdict::truncated:
        return "truncated";
    case Verdict::targetOutOfRange:
        return "target_out_of_range";
    case Verdict::targetPoisoned:
        return "target_poisoned";
    case Verdict::tooManyTargets:
        return "too_many_targets";
    case Verdict::payloadTooLong:
        return "payload_too_long";
    }
    return "unknown";
}

/** Validates one msg-19 body as far as its wire shape allows. */
Verdict validate(std::span<const std::byte> payload, Incident& parsed) noexcept {
    parsed = {};
    encoding::bits::Reader reader(payload);

    std::uint64_t field = 0;
    if (!reader.read(kTargetWidth, field)) {
        return Verdict::truncated;
    }
    parsed.primaryTarget = static_cast<std::uint32_t>(field);
    Verdict verdict = Verdict::accepted;
    if (!target_allowed(parsed.primaryTarget, verdict)) {
        return verdict;
    }

    if (!reader.read(kExtraCountWidth, field)) {
        return Verdict::truncated;
    }
    parsed.extraTargetCount = static_cast<std::uint32_t>(field);
    if (parsed.extraTargetCount > kExtraTargetMaximum) {
        return Verdict::tooManyTargets;
    }
    for (std::uint32_t index = 0; index < parsed.extraTargetCount; ++index) {
        if (!reader.read(kTargetWidth, field)) {
            return Verdict::truncated;
        }
        parsed.extraTargets[index] = static_cast<std::uint32_t>(field);
        if (!target_allowed(parsed.extraTargets[index], verdict)) {
            return verdict;
        }
    }

    if (!reader.read(kSelectorPresenceWidth, field)) {
        return Verdict::truncated;
    }
    if (field != 0) {
        // Every target is checked by now, which is the part that can crash the Client.
        parsed.hasCompressedSelector = true;
        return Verdict::accepted;
    }

    if (!reader.read(kOptionalPresenceWidth, field)) {
        return Verdict::truncated;
    }
    if (field != 0 && !reader.skip(kOptionalFieldWidth)) {
        return Verdict::truncated;
    }

    if (!reader.read(kPayloadLengthWidth, field)) {
        return Verdict::truncated;
    }
    parsed.payloadLength = static_cast<std::uint32_t>(field);
    if (parsed.payloadLength > kPayloadMaximum) {
        return Verdict::payloadTooLong;
    }
    if (reader.remaining_bits() < static_cast<std::size_t>(parsed.payloadLength) * CHAR_BIT) {
        return Verdict::truncated;
    }
    parsed.hasPayload = true;
    return Verdict::accepted;
}

} // namespace sunrise::middleware::bap::activity_message::incident
