#include "presence_record_encoder.h"

#include <algorithm>
#include <cstring>

namespace sunrise::middleware::datagen::presence {
namespace {
template <typename T>
void put(std::span<std::byte> output, std::size_t offset, const T& value) noexcept {
    std::memcpy(output.data() + offset, &value, sizeof value);
}
template <std::size_t N>
void text(std::span<std::byte> output,
          std::size_t offset,
          const std::array<char, N>& source) noexcept {
    for (std::size_t i = 0; i + 1 < N && source[i] != '\0'; ++i) {
        const auto unit = static_cast<std::uint16_t>(static_cast<unsigned char>(source[i]));
        put(output, offset + i * 2, unit);
    }
}
} // namespace

bool encode_directory(std::uint64_t accountSoid,
                      std::uint64_t characterSoid,
                      const state::AccountPresence& presence,
                      std::span<std::byte> output) noexcept {
    if (accountSoid == 0 || output.size() < kDirectorySize) {
        return false;
    }
    auto body = output.first(kDirectorySize);
    std::fill(body.begin(), body.end(), std::byte{});
    put(body, 0, accountSoid);
    put(body, 8, characterSoid);
    text(body, 0x10, presence.displayName);
    text(body, 0x46, presence.nameCode);
    body[0x59] = static_cast<std::byte>(presence.flags & state::kPresenceFlagsMask);
    return true;
}

/** Native family-two member schema, adapted from the cleaned reference at ac0c939d. */
bool encode_member(const Member& member, std::span<std::byte> output) noexcept {
    if (member.characterSoid == 0 || output.size() < kMemberSize) {
        return false;
    }
    auto body = output.first(kMemberSize);
    std::fill(body.begin(), body.end(), std::byte{});
    put(body, 0, member.characterSoid);
    put(body, 8, member.previousActivityIndex);
    put(body, 0x0A, member.activityIndex);
    put(body, 0x0D, member.titleKind);
    const auto level = static_cast<std::int32_t>(member.level);
    const auto light = static_cast<float>(member.light);
    put(body, 0x10, level);
    put(body, 0x14, member.light);
    put(body, 0x18, light);
    put(body, 0x20, member.light);
    put(body, 0x24, member.emblem);
    put(body, 0x26, kAbsentDefinition);
    put(body, 0x28, kAbsentDefinition);
    put(body, 0x34, member.title);
    body[0x38] = body[0x39] = body[0x3A] = static_cast<std::byte>(member.level);
    put(body, 0x40, member.groupKey);
    put(body, 0x44, member.memberCount);
    return true;
}

} // namespace sunrise::middleware::datagen::presence
