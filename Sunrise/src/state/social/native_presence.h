#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::social {

inline constexpr std::size_t kNativeJoinDescriptorSize = 128;
inline constexpr std::size_t kNativeFireteamSize = 0xB60;

/** Public fields decoded from the native character writeback, excluding inventory and unlocks. */
struct NativePresence {
    bool published{};
    /** Local selected character at the writeback's commit, not a payload-supplied owner. */
    std::uint64_t characterSoid{};
    bool hasGroup{};
    std::uint32_t groupKey{};
    std::int8_t memberCount{};
    bool hasFireteam{};
    std::array<std::byte, kNativeFireteamSize> fireteam{};
    std::uint8_t descriptorSize{};
    std::array<std::byte, kNativeJoinDescriptorSize> descriptor{};

    bool operator==(const NativePresence&) const = default;
};

[[nodiscard]] inline bool valid(const NativePresence& value) noexcept {
    if (!value.published) {
        return value == NativePresence{};
    }
    if ((!value.hasGroup && (value.groupKey != 0 || value.memberCount != 0))
        || value.descriptorSize > value.descriptor.size()) {
        return false;
    }
    if (!value.hasFireteam
        && std::any_of(value.fireteam.begin(), value.fireteam.end(), [](std::byte byte) {
               return byte != std::byte{};
           })) {
        return false;
    }
    return std::all_of(value.descriptor.begin() + value.descriptorSize,
                       value.descriptor.end(),
                       [](std::byte byte) { return byte == std::byte{}; });
}

} // namespace sunrise::state::social
