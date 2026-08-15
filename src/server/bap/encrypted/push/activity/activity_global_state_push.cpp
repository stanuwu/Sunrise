#include "activity_global_state_push.h"

#include <Windows.h>

#include <algorithm>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/activity_global_state_encoder.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/destination/activity_destination_spawn_binding.h"
#include "../../../../../state/build_data/runtime.h"
#include "activity_arrival.h"
#include "activity_notification_frame.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace message = middleware::bap::activity_message::global_activity_state;

namespace {

/** The byte a bubble carries when its first slice-set state is enabled. */
constexpr std::uint8_t kBubbleEnabledByte = 0x80;

/**
 * Copies one destination name into the fixed message field.
 * @param selection Destination carrying the package name.
 * @param state Receives the name and its length.
 */
void copy_name(const state::activity::destination::DestinationSelection& selection,
               message::GlobalActivityState& state) noexcept {
    // The last element is reserved for the null, so a full-width name loses its last byte instead
    // of failing the encode.
    const std::size_t length = selection.packageNameLength < message::kNameCapacity
                                   ? selection.packageNameLength
                                   : message::kNameCapacity - 1;
    for (std::size_t index = 0; index < length; ++index) {
        state.name[index] = static_cast<char>(selection.packageName[index]);
    }
    state.nameLength = length;
}

} // namespace

/** Builds the whole message body input for one session. */
[[nodiscard]] bool
resolve_state(std::uint64_t sessionId,
              message::GlobalActivityState& output,
              state::activity::destination::DestinationSelection& selection) noexcept {
    output = {};
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    const state::activity::defaults::FallbackPolicy& fallback =
        defaults.defaultDestination.fallback;

    // The session's own destination wins. The authored default covers a session that committed
    // before any selection was readable.
    if (!state::activity::destination::snapshot(sessionId, selection)) {
        selection = defaults.defaultDestination.selection;
    }
    copy_name(selection, output);
    // The descriptor view points into caller storage that outlives the encode.
    output.descriptorBits = std::span<const std::byte>(selection.descriptorBits);
    output.descriptorBitLength = selection.descriptorBitLength;
    output.reason = selection.reason;
    output.fromActivityIndex = selection.previousActivityIndex;
    output.activityIndex = selection.activityIndex;
    output.spawnSetHash =
        state::activity::destination::attachable_spawn_set_hash(selection, fallback.spawnSetHash);

    // The extracted layout wins where the packages carry one. The count and the output array must
    // come from the same source: a count from one and states from another is how uniform values
    // reach the wire and look as though they worked.
    const std::string_view name(output.name.data(), output.nameLength);
    ::sunrise::state::build_data::scenarios::Definition layout{};
    if (::sunrise::state::build_data::find_scenario_layout(name, layout)) {
        output.bubbleCount = layout.bubbleCount;
        std::copy(
            layout.bubbleStates.begin(), layout.bubbleStates.end(), output.bubbleStates.begin());
        output.hasSliceSet = true;
        output.sliceSetIndex =
            arrival_slice_set(defaults.defaultDestination, selection, name, layout);
        return true;
    }
    output.bubbleCount = fallback.bubbleCount;
    for (std::size_t index = 0; index < message::kBubbleStateCount; ++index) {
        const bool stateful = (fallback.statefulBubbleMask >> index & 1U) != 0;
        output.bubbleStates[index] = stateful ? kBubbleEnabledByte : message::kBubbleStateNone;
    }
    output.hasSliceSet = true;
    // No layout means no bubble array to derive an arrival from, so the authored index stands.
    output.sliceSetIndex = fallback.initialSliceSet;
    return true;
}

/** Appends one global-activity-state svc9 notification and advances its local nonce. */
bool append_global_state_notification(Scratch& scratch,
                                      std::uint64_t sessionId,
                                      std::span<const std::byte, state::kAesKeySize> key,
                                      std::array<std::byte, state::kBapNonceSize>& nonce,
                                      std::span<std::byte> response,
                                      std::size_t& written) noexcept {
    message::GlobalActivityState body{};
    state::activity::destination::DestinationSelection selection{};
    if (written > response.size() || !resolve_state(sessionId, body, selection)) {
        return false;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const bool encoded =
        message::encode_global_activity_state(body, scratch.responseBody, messageSize)
        && append_notification_frame(scratch,
                                     sessionId,
                                     message::kMessageType,
                                     std::span(scratch.responseBody).first(messageSize),
                                     key,
                                     nonce,
                                     response,
                                     written);
    // A replayed descriptor makes the body wider than the minimum encoding, so the clear uses the
    // size actually produced, not that minimum.
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    SecureZeroMemory(&body, sizeof body);
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push::activity
