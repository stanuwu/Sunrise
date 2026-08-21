/* Included once by state_account_identity_runtime.cpp. */

#include "../../core/settings/character_persistence.h"
#include "character_deletion.h"

namespace sunrise::state {
namespace {

[[nodiscard]] bool same_pending_deletion(const PendingCharacterDeletion& left,
                                         const PendingCharacterDeletion& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.beforeCharacterCount == right.beforeCharacterCount
           && left.characterIndex == right.characterIndex
           && runtime::detail::same_character(left.deletedCharacter, right.deletedCharacter);
}

[[nodiscard]] CharacterDeletionResult
stage_character_deletion(const AccountState& before,
                         std::uint64_t characterSoid,
                         PendingCharacterDeletion& mutation,
                         AccountState& after) noexcept {
    mutation = {};
    after = {};
    if (characterSoid == 0 || !account::valid(before) || before.primarySoid == 0) {
        return CharacterDeletionResult::invalid;
    }

    std::size_t characterIndex = before.characterCount;
    for (std::size_t index = 0; index < before.characterCount; ++index) {
        if (before.characters[index].soid != characterSoid) {
            continue;
        }
        if (characterIndex != before.characterCount) {
            return CharacterDeletionResult::invalid;
        }
        characterIndex = index;
    }
    if (characterIndex == before.characterCount) {
        return CharacterDeletionResult::notFound;
    }

    after = before;
    const CharacterState deleted = after.characters[characterIndex];
    for (std::size_t index = characterIndex; index + 1U < after.characterCount; ++index) {
        after.characters[index] = after.characters[index + 1U];
    }
    --after.characterCount;
    after.characters[after.characterCount] = {};

    if (!account::valid(after)) {
        return CharacterDeletionResult::invalid;
    }

    mutation.deletedCharacter = deleted;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = characterSoid;
    mutation.beforeCharacterCount = before.characterCount;
    mutation.characterIndex = characterIndex;
    mutation.prepared = true;
    return CharacterDeletionResult::ok;
}

} // namespace

CharacterDeletionResult prepare_character_deletion(std::uint64_t characterSoid,
                                                    PendingCharacterDeletion& mutation) noexcept {
    AccountState ignored{};
    return stage_character_deletion(account_snapshot(), characterSoid, mutation, ignored);
}

bool preview_character_deletion(const PendingCharacterDeletion& mutation,
                                AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared) {
        return false;
    }
    PendingCharacterDeletion canonical{};
    const CharacterDeletionResult result =
        stage_character_deletion(account_snapshot(), mutation.characterSoid, canonical, after);
    return result == CharacterDeletionResult::ok && same_pending_deletion(canonical, mutation);
}

bool commit_character_deletion(PendingCharacterDeletion& mutation) noexcept {
    const PendingCharacterDeletion prepared = mutation;
    mutation = {};
    if (!prepared.prepared) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    PendingCharacterDeletion canonical{};
    AccountState after{};
    const AccountState before = runtime::storage::g_state.account;
    const CharacterDeletionResult result =
        stage_character_deletion(before, prepared.characterSoid, canonical, after);
    bool committed = result == CharacterDeletionResult::ok
                     && same_pending_deletion(canonical, prepared) && account::valid(after);

    // Keep disk and authoritative runtime State on one side of the same boundary. When a prior
    // failed run left settings.json out of sync, persistence repairs it to this canonical after
    // image rather than deleting whichever stale row happens to share the requested SOID.
    if (committed) {
        committed = core::settings::persistence::remove_character(
            before, after, prepared.characterSoid, prepared.characterIndex);
    }
    if (committed) {
        runtime::storage::g_state.account = after;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

const char* character_deletion_result_name(CharacterDeletionResult result) noexcept {
    switch (result) {
    case CharacterDeletionResult::ok:
        return "ok";
    case CharacterDeletionResult::notFound:
        return "not_found";
    case CharacterDeletionResult::invalid:
    default:
        return "invalid";
    }
}

} // namespace sunrise::state
