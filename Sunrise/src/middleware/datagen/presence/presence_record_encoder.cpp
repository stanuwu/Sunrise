#include "presence_record_encoder.h"

#include <algorithm>
#include <cstring>

namespace sunrise::middleware::datagen::presence {
namespace {
// Directory record field positions. Both names are UTF-16, two bytes per code unit, and the
// display name is bounded by the name code that follows it.
constexpr std::size_t kDirectoryAccountSoid = 0;
constexpr std::size_t kDirectoryCharacterSoid = 8;
constexpr std::size_t kDirectoryDisplayName = 0x10;
constexpr std::size_t kDirectoryNameCode = 0x46;
constexpr std::size_t kDirectoryFlags = 0x59;
static_assert(state::kDisplayNameCapacity == (kDirectoryNameCode - kDirectoryDisplayName) / 2);

// Native family-two member schema field positions, including its three repeated level bytes.
constexpr std::size_t kMemberCharacterSoid = 0;
constexpr std::size_t kMemberPreviousActivity = 8;
constexpr std::size_t kMemberActivity = 0x0A;
constexpr std::size_t kMemberTitleKind = 0x0D;
constexpr std::size_t kMemberLevel = 0x10;
constexpr std::size_t kMemberLight = 0x14;
constexpr std::size_t kMemberLightFloat = 0x18;
constexpr std::size_t kMemberDisplayedLight = 0x20;
constexpr std::size_t kMemberEmblem = 0x24;
constexpr std::size_t kMemberAbsentA = 0x26;
constexpr std::size_t kMemberAbsentB = 0x28;
constexpr std::size_t kMemberTitle = 0x34;
constexpr std::size_t kMemberLevelA = 0x38;
constexpr std::size_t kMemberLevelB = 0x39;
constexpr std::size_t kMemberLevelC = 0x3A;
constexpr std::size_t kMemberGroupKey = 0x40;
constexpr std::size_t kMemberMemberCount = 0x44;

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
    put(body, kDirectoryAccountSoid, accountSoid);
    put(body, kDirectoryCharacterSoid, characterSoid);
    text(body, kDirectoryDisplayName, presence.displayName);
    text(body, kDirectoryNameCode, presence.nameCode);
    body[kDirectoryFlags] = static_cast<std::byte>(presence.flags & state::kPresenceFlagsMask);
    return true;
}

bool encode_member(const Member& member, std::span<std::byte> output) noexcept {
    if (member.characterSoid == 0 || output.size() < kMemberSize) {
        return false;
    }
    auto body = output.first(kMemberSize);
    std::fill(body.begin(), body.end(), std::byte{});
    put(body, kMemberCharacterSoid, member.characterSoid);
    put(body, kMemberPreviousActivity, member.previousActivityIndex);
    put(body, kMemberActivity, member.activityIndex);
    put(body, kMemberTitleKind, member.titleKind);
    const auto level = static_cast<std::int32_t>(member.level);
    const auto light = static_cast<float>(member.light);
    put(body, kMemberLevel, level);
    put(body, kMemberLight, member.light);
    put(body, kMemberLightFloat, light);
    put(body, kMemberDisplayedLight, member.light);
    put(body, kMemberEmblem, member.emblem);
    put(body, kMemberAbsentA, kAbsentDefinition);
    put(body, kMemberAbsentB, kAbsentDefinition);
    put(body, kMemberTitle, member.title);
    body[kMemberLevelA] = body[kMemberLevelB] = body[kMemberLevelC] =
        static_cast<std::byte>(member.level);
    put(body, kMemberGroupKey, member.groupKey);
    put(body, kMemberMemberCount, member.memberCount);
    return true;
}

} // namespace sunrise::middleware::datagen::presence
