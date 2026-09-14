#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/account/account_presence.h"

namespace sunrise::middleware::datagen::presence {

inline constexpr std::size_t kDirectorySize = 96;
inline constexpr std::size_t kMemberSize = 80;
inline constexpr std::uint16_t kAbsentDefinition = 0xFFFF;

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

[[nodiscard]] bool encode_directory(std::uint64_t accountSoid,
                                    std::uint64_t characterSoid,
                                    const state::AccountPresence& presence,
                                    std::span<std::byte> output) noexcept;
[[nodiscard]] bool encode_member(const Member& member, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::presence
