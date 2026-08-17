#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sunrise::client::content::entity_names::localized_aliases {

inline constexpr std::size_t kNameCapacity = 128;

struct Entry {
    std::uint32_t tag{};
    std::array<char, kNameCapacity> text{};
    std::uint8_t length{};
};

struct Result {
    std::size_t wrappers{};
    std::size_t placements{};
    std::size_t resolved{};
};

[[nodiscard]] bool append(std::wstring_view packageDirectory,
                          std::vector<Entry>& output,
                          Result& result) noexcept;

} // namespace sunrise::client::content::entity_names::localized_aliases
