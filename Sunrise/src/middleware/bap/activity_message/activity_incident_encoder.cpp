#include <algorithm>
#include <climits>

#include "incident.h"

namespace sunrise::middleware::bap::activity_message::incident {
namespace {

[[nodiscard]] bool target_allowed(std::uint32_t target) noexcept {
    return target <= kTargetMaximum
           && std::find(kPoisonTargets.begin(), kPoisonTargets.end(), target)
                  == kPoisonTargets.end();
}

/** @return True when every bounded incident field can be represented on the wire. */
[[nodiscard]] bool valid(const Incident& incident) noexcept {
    if (!target_allowed(incident.primaryTarget) || incident.extraTargetCount > kExtraTargetMaximum
        || incident.selectorLength > kSelectorMaximum || incident.payloadLength > kPayloadMaximum
        || (!incident.hasCompressedSelector && incident.selectorLength != 0)
        || (!incident.hasOptionalBlock
            && (incident.optionalWordA != 0 || incident.optionalWordB != 0))) {
        return false;
    }
    for (std::uint32_t index = 0; index < incident.extraTargetCount; ++index) {
        if (!target_allowed(incident.extraTargets[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool write_bytes(encoding::bits::Writer& writer,
                               std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) {
        if (!writer.write(std::to_integer<std::uint8_t>(value), CHAR_BIT)) {
            return false;
        }
    }
    return true;
}

/** Writes fields after the caller has established semantic validity. */
[[nodiscard]] bool write_fields(encoding::bits::Writer& writer, const Incident& incident) noexcept {
    bool written = writer.write(incident.primaryTarget, kTargetWidth)
                   && writer.write(incident.extraTargetCount, kExtraCountWidth);
    for (std::uint32_t index = 0; written && index < incident.extraTargetCount; ++index) {
        written = writer.write(incident.extraTargets[index], kTargetWidth);
    }
    written =
        written && writer.write(incident.hasCompressedSelector ? 1U : 0U, kSelectorPresenceWidth);
    if (written && incident.hasCompressedSelector) {
        written =
            writer.write(incident.selectorLength, kSelectorLengthWidth)
            && write_bytes(writer, std::span(incident.selector).first(incident.selectorLength));
    }
    written = written && writer.write(incident.hasOptionalBlock ? 1U : 0U, kOptionalPresenceWidth);
    if (written && incident.hasOptionalBlock) {
        written = writer.write(incident.optionalWordA, kOptionalWordWidth)
                  && writer.write(incident.optionalWordB, kOptionalWordWidth);
    }
    return written && writer.write(incident.payloadLength, kPayloadLengthWidth)
           && write_bytes(writer, std::span(incident.payload).first(incident.payloadLength));
}

} // namespace

/** Writes one bounded incident body after a complete semantic preflight. */
bool write(encoding::bits::Writer& writer, const Incident& incident) noexcept {
    return valid(incident) && write_fields(writer, incident);
}

} // namespace sunrise::middleware::bap::activity_message::incident
