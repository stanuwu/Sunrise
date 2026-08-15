#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/internal.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {

/**
 * Appends the opcode-504 Family-4 move as one increment above the peer's current version.
 * @param scratch Lock-owned transform buffers.
 * @param select Staged after-image, object definitions, and both character keys.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce after the correlated svc-11 response.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after the complete push is appended.
 * @return True when the 3 object operations and the whole svc-123 frame fit.
 */
bool append_select_character_notification(Scratch& scratch,
                                          const queuez::SelectCharacter& select,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::span<const std::byte, state::kBapNonceSize> nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_selection_move(scratch, select, prepared)) {
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        return false;
    }
    queuez_report::push(
        "select", queuez::kAccountFamilyType, objectCount, written - beforeBytes, 1);
    return true;
}

} // namespace sunrise::server::bap::encrypted::push
