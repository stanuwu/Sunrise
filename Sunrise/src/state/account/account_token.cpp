#include "account_token.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace sunrise::state::account {
namespace {

// Two fixed fork markers, ASCII "SUNRISEA" and "SUNRISED". Neither is a retail value and neither
// is secret; they only keep this generator's two streams apart.
constexpr std::uint64_t kLegacySeed = 0x53554E5249534541ULL;
/** The 64-bit golden-ratio odd multiplier, which spreads one soid across the whole word. */
constexpr std::uint64_t kLegacyMultiplier = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kKeyMask = 0x53554E5249534544ULL;
constexpr unsigned kByteWidth = 8;

/** One 13/7/17 xorshift64 step; bits 24-31 preserve the legacy token's byte selection. */
[[nodiscard]] std::byte next_byte(std::uint64_t& x) noexcept {
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    return static_cast<std::byte>(x >> 24U);
}

[[nodiscard]] std::uint64_t seed_for(std::uint64_t primarySoid) noexcept {
    // The OR keeps the xorshift out of its single absorbing state for any key.
    return (kLegacySeed ^ (primarySoid * kLegacyMultiplier)) | 1ULL;
}

void legacy_session_token(std::uint64_t primarySoid, std::span<std::byte> output) noexcept {
    if (output.size() < kSignOnTokenSize) {
        return;
    }
    std::uint64_t state = seed_for(primarySoid);
    for (std::size_t index = 0; index < kSignOnTokenSize; ++index) {
        output[index] = next_byte(state);
    }
}

} // namespace

void signon_token(std::uint64_t primarySoid, std::span<std::byte> output) noexcept {
    if (primarySoid == 0 || output.size() < kSignOnTokenSize) {
        return;
    }
    legacy_session_token(primarySoid, output);
    // The tail stays exactly the legacy derivation's, so it is a pure function of the key and
    // checks the complete portable marker. This derivation is not credential authentication.
    const std::uint64_t masked = primarySoid ^ kKeyMask;
    for (std::size_t index = 0; index < kSignOnTokenKeyBytes; ++index) {
        output[index] = static_cast<std::byte>((masked >> (index * kByteWidth)) & 0xFFULL);
    }
}

std::uint64_t soid_from_signon_token(std::span<const std::byte> token) noexcept {
    if (token.size() != kSignOnTokenSize) {
        return 0;
    }
    std::uint64_t masked = 0;
    for (std::size_t index = 0; index < kSignOnTokenKeyBytes; ++index) {
        masked |= static_cast<std::uint64_t>(std::to_integer<unsigned>(token[index]))
                  << (index * kByteWidth);
    }
    const std::uint64_t candidate = masked ^ kKeyMask;
    if (candidate == 0) {
        return 0;
    }
    std::array<std::byte, kSignOnTokenSize> expected{};
    signon_token(candidate, expected);
    // Whole-token comparison rather than tail-only: the head is a function of the candidate too,
    // so a forged head can only agree with itself, and one comparison covers both halves.
    return std::equal(expected.begin(), expected.end(), token.begin()) ? candidate : 0;
}

} // namespace sunrise::state::account
