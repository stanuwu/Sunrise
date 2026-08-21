#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../account/account_state.h"

namespace sunrise::state {

/** Result of preparing one native character-creator transaction. */
enum class CharacterCreationResult : std::uint8_t {
    ok,
    full,
    missingTemplate,
    invalid,
};

/** Native opcode-501 data retained without assigning semantics to its authored blocks. */
struct NativeCharacterCreation final {
    CharacterRace race{CharacterRace::human};
    CharacterGender gender{CharacterGender::male};
    CharacterClass characterClass{CharacterClass::titan};
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCharacterCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCharacterCreationTailSize> creationTail{};
    std::uint8_t creatorTrailer{};
};

/**
 * Checked template-backed character creation retained until every QueueZ after-image fits.
 * The request itself is kept so preview and commit can independently rebuild the canonical result.
 */
struct PendingCharacterCreation final {
    NativeCharacterCreation creation{};
    CharacterState createdCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t beforeCharacterCount{};
    std::size_t characterIndex{};
    /** Existing rosters follow the measured create -> select flow; the first real Guardian does not. */
    bool selectCreated{};
    bool prepared{};
};

/** Builds a character creation after-image without publishing account State. */
[[nodiscard]] CharacterCreationResult
prepare_character_creation(const NativeCharacterCreation& creation,
                           PendingCharacterCreation& mutation) noexcept;

/** Materializes the exact uncommitted account image represented by a prepared creation. */
[[nodiscard]] bool preview_character_creation(const PendingCharacterCreation& mutation,
                                              AccountState& after) noexcept;

/** Commits a prepared creation only when rebuilding it against current State is still identical. */
[[nodiscard]] bool commit_character_creation(PendingCharacterCreation& mutation) noexcept;

/** Stable diagnostic name for one creation result. */
[[nodiscard]] const char* character_creation_result_name(CharacterCreationResult result) noexcept;


} // namespace sunrise::state
