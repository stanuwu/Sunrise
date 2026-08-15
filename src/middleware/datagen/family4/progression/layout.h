#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::middleware::datagen::family4::progression::layout {

/** A native progression row has 12 value bytes before its definition index. */
inline constexpr std::size_t kValueSize = 12;

#pragma pack(push, 1)

/** One fixed native progression row whose definition index follows 3 value lanes. */
struct Entry {
    std::array<std::byte, kValueSize> values{};
    std::uint16_t definitionIndex{};
    std::uint16_t reserved{};
};

#pragma pack(pop)

static_assert(sizeof(Entry) == kValueSize * sizeof(std::byte) + 2 * sizeof(std::uint16_t));

} // namespace sunrise::middleware::datagen::family4::progression::layout
