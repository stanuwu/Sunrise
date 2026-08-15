#include <array>

#include "../../../../../middleware/datagen/family4/account/selection_patch/\
account_selection_patch_encoder.h"
#include "../../queuez/queuez_state_validation.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {

namespace selection_patch = middleware::datagen::family4::account::selection_patch;

/**
 * Appends the opcode-505 account-selection patch as a Family-4 increment.
 * @param scratch Lock-owned transform buffers.
 * @param change Staged queuez after-image and account definition.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce after the correlated svc-11 response.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after the complete push is appended.
 * @return True when the 17-byte patch and authenticated svc-123 frame fit.
 */
bool append_change_character_notification(Scratch& scratch,
                                          const queuez::ChangeCharacter& change,
                                          std::span<const std::byte, state::kAesKeySize> key,
                                          std::span<const std::byte, state::kBapNonceSize> nonce,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    std::size_t patchSize = 0;
    if (!selection_patch::encode(0, scratch.plaintext, patchSize)
        || patchSize != selection_patch::kPayloadSize) {
        queuez_frame::clear_object_storage(scratch, patchSize, 0);
        return false;
    }
    const middleware::queuez::Object object{
        change.accountDefinitionId,
        change.after.family4RootSoid,
        middleware::queuez::Encoding::tagReflection,
        std::span(scratch.plaintext).first(patchSize),
    };
    const std::array objects{object};
    const middleware::queuez::Family family{
        queuez::kAccountFamilyType,
        change.after.family4RootSoid,
        change.after.family4Version,
        0,
        objects,
    };
    return queuez_frame::append(scratch, family, patchSize, 0, key, nonce, response, written);
}

} // namespace sunrise::server::bap::encrypted::push
