#pragma once

#include <cstddef>
#include <cstdint>

#include "../account/account_state.h"

namespace sunrise::state {

/** Result of preparing one native delete-character transaction. */
enum class CharacterDeletionResult : std::uint8_t {
    ok,
    notFound,
    invalid,
};

/**
 * Checked character removal retained until the correlated response has been staged.
 * The exact deleted row and dense index are kept as a staleness guard for the final commit.
 */
struct PendingCharacterDeletion final {
    CharacterState deletedCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t beforeCharacterCount{};
    std::size_t characterIndex{};
    bool prepared{};
};

/** Builds a dense account after-image with the requested character removed. */
[[nodiscard]] CharacterDeletionResult
prepare_character_deletion(std::uint64_t characterSoid,
                           PendingCharacterDeletion& mutation) noexcept;

/** Materializes the exact uncommitted account image represented by a prepared deletion. */
[[nodiscard]] bool preview_character_deletion(const PendingCharacterDeletion& mutation,
                                              AccountState& after) noexcept;

/**
 * Removes the character from settings.json and runtime State as one commit boundary.
 * The settings rewrite is completed first while the State write lock is held; State is only
 * replaced after that durable rewrite succeeds.
 */
[[nodiscard]] bool commit_character_deletion(PendingCharacterDeletion& mutation) noexcept;

/** Stable diagnostic name for one deletion result. */
[[nodiscard]] const char* character_deletion_result_name(CharacterDeletionResult result) noexcept;

} // namespace sunrise::state
