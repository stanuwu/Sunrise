#include "queuez_update_frame.h"

#include <Windows.h>

#include <algorithm>
#include <array>

#include "../../../../../middleware/bap/frame.h"
#include "../../../../../middleware/secure_channel/runtime.h"

namespace sunrise::server::bap::encrypted::push::queuez_frame {
namespace {

/** Unsolicited notifications have no request sequence to echo. */
constexpr std::uint32_t kNotificationSequence = 0;

/**
 * Wipes the part of one scratch buffer that may hold written bytes.
 * @param buffer Lock-owned scratch storage.
 * @param size Largest prefix that may hold transformed bytes.
 */
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

} // namespace

/** Securely clears raw and compressed object-staging prefixes. */
void clear_object_storage(Scratch& scratch,
                          std::size_t rawClearSize,
                          std::size_t compressedClearSize) noexcept {
    clear_prefix(scratch.plaintext, rawClearSize);
    clear_prefix(scratch.sealed, compressedClearSize);
}

/** Appends one prepared family as a complete authenticated svc-123 frame. */
bool append(Scratch& scratch,
            const middleware::queuez::Family& family,
            std::size_t rawClearSize,
            std::size_t compressedClearSize,
            std::span<const std::byte, state::kAesKeySize> key,
            std::span<const std::byte, state::kBapNonceSize> nonce,
            std::span<std::byte> response,
            std::size_t& written) noexcept {
    /** A staged queuez notification carries exactly one prepared family. */
    constexpr std::size_t familyCount = 1;
    const std::array families{family};
    std::size_t bodySize = 0;
    std::size_t payloadSize = 0;
    std::size_t sealedSize = 0;
    std::size_t frameSize = 0;
    const bool updateEncoded =
        written <= response.size()
        && middleware::queuez::encode_update(
            std::span(families).first(familyCount), scratch.responseBody, bodySize);
    // The update owns a byte copy, so borrowed object payloads can be erased before encryption.
    clear_object_storage(scratch, rawClearSize, compressedClearSize);
    const bool encoded =
        updateEncoded
        && middleware::bap::encode_notification_payload(
            middleware::bap::NotificationService::queuezUpdate,
            kNotificationSequence,
            std::span(scratch.responseBody).first(bodySize),
            scratch.responsePayload,
            payloadSize)
        && middleware::secure_channel::seal_frame(
            key,
            nonce,
            std::span(scratch.responsePayload).first(payloadSize),
            scratch.sealed,
            sealedSize)
        && middleware::bap::encode_frame(middleware::bap::FrameType::encrypted,
                                         std::span(scratch.sealed).first(sealedSize),
                                         response.subspan(written),
                                         frameSize);
    if (encoded) {
        written += frameSize;
    }
    clear_prefix(scratch.responseBody, bodySize);
    clear_prefix(scratch.responsePayload, payloadSize);
    clear_prefix(scratch.sealed, payloadSize + middleware::secure_channel::kFrameTagSize);
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push::queuez_frame
