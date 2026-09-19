#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/account/account_presence.h"
#include "../character_record/layout.h"
#include "../definitions.h"

namespace sunrise::middleware::datagen::presence {

/** The two family-two slot descriptor sizes these encoders fill. */
inline constexpr std::size_t kDirectorySize = kSocialRosterDirectorySize;
/** See kDirectorySize. */
inline constexpr std::size_t kMemberSize = kSocialRosterMemberSize;
/** The sentinel every definition-index field carries when the member has no such entry. */
inline constexpr std::uint16_t kAbsentDefinition = character_record::layout::kEmptyDefinitionIndex;

/** Values supplied by this member's account and native presence publishers. */
struct Member {
    std::uint64_t characterSoid{};
    std::uint8_t level{};
    std::int32_t light{};
    std::int16_t activityIndex{-1};
    std::int16_t previousActivityIndex{-1};
    std::uint32_t groupKey{};
    std::int8_t memberCount{};
    std::uint16_t emblem{kAbsentDefinition};
    std::uint16_t title{kAbsentDefinition};
    std::int8_t titleKind{};
};

/** @return False for a zero account soid, or an `output` smaller than `kDirectorySize`. */
[[nodiscard]] bool encode_directory(std::uint64_t accountSoid,
                                    std::uint64_t characterSoid,
                                    const state::AccountPresence& presence,
                                    std::span<std::byte> output) noexcept;
/** @return False for a zero character soid, or an `output` smaller than `kMemberSize`. */
[[nodiscard]] bool encode_member(const Member& member, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::presence
