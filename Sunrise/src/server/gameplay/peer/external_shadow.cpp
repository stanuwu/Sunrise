#include "external_shadow.h"

namespace sunrise::server::gameplay::peer {
namespace {

namespace shadow = state::gameplay::external;

/** Reports whether a context has every required process-local generation. */
[[nodiscard]] bool valid_context(const shadow::ExternalShadowKey& key) noexcept {
    return key.carrier.generation != 0 && key.authenticatedMemberId != 0 && key.peerGeneration != 0
           && key.channelGeneration != 0 && key.groupSessionId != 0 && key.viewGeneration != 0
           && key.activitySessionId != 0;
}

/** Reports whether two observations belong to the same peer and view. */
[[nodiscard]] bool same_context(const shadow::ExternalShadowKey& left,
                                const shadow::ExternalShadowKey& right) noexcept {
    return left.carrier.kind == right.carrier.kind
           && left.carrier.generation == right.carrier.generation
           && left.authenticatedMemberId == right.authenticatedMemberId
           && left.peerGeneration == right.peerGeneration
           && left.channelGeneration == right.channelGeneration
           && left.groupSessionId == right.groupSessionId
           && left.viewGeneration == right.viewGeneration
           && left.activitySessionId == right.activitySessionId
           && left.remoteConnectionSequence == right.remoteConnectionSequence
           && left.localConnectionSequence == right.localConnectionSequence;
}

/** Copies the empty-channel fields carried on every observation. */
void copy_channel_state(const middleware::gameplay::external::EmptyProfile& decoded,
                        shadow::ExternalShadow& output) noexcept {
    output.defaultBubble = decoded.defaultBubble;
    output.defaultBubblePresent = decoded.defaultBubblePresent;
    output.channel3TrailingList = decoded.channel3TrailingList;
}

} // namespace

/** Reads one complete empty-channel frame without changing output on failure. */
middleware::gameplay::external::EmptyProfileResult
read_empty_frame(middleware::encoding::bits::Reader& reader, EmptyFrame& output) noexcept {
    namespace external = middleware::gameplay::external;
    EmptyFrame candidate{};
    const external::EmptyProfileResult profile =
        external::read_empty_profile(reader, candidate.profile);
    if (profile != external::EmptyProfileResult::accepted) {
        return profile;
    }
    if (!middleware::gameplay::peer::read_filler_and_padding(reader, candidate.filler)
        || reader.remaining_bits() != 0) {
        return external::EmptyProfileResult::malformed;
    }
    output = candidate;
    return external::EmptyProfileResult::accepted;
}

/** Prepares one receive-only observation without changing current state. */
ShadowPrepareResult
prepare_external_shadow(const shadow::ExternalShadowKey& currentContext,
                        const middleware::gameplay::external::EmptyProfile& decoded,
                        const shadow::ExternalShadow& current,
                        shadow::ExternalShadow& next) noexcept {
    if (!valid_context(currentContext)) {
        return ShadowPrepareResult::unsupportedCommon;
    }

    shadow::ExternalShadow candidate{};
    if (decoded.commonPresent) {
        if (decoded.common.entryCount != 1
            || decoded.common.entries[0].activitySessionId != currentContext.activitySessionId) {
            return ShadowPrepareResult::unsupportedCommon;
        }
        candidate.key = currentContext;
        candidate.patchEpoch = decoded.common.patchEpoch;
        candidate.reconciliationGeneration = decoded.common.entries[0].reconciliationGeneration;
        candidate.occupied = true;
    } else {
        if (!current.occupied || !same_context(current.key, currentContext)) {
            return ShadowPrepareResult::missingCommon;
        }
        candidate = current;
    }
    copy_channel_state(decoded, candidate);
    next = candidate;
    return ShadowPrepareResult::ready;
}

} // namespace sunrise::server::gameplay::peer
