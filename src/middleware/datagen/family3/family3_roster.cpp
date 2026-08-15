#include "family3_roster.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "../roster/queuez_roster.h"

namespace sunrise::middleware::datagen::family3 {
namespace {

/** The first trailing index bank holds 27 definition indices. */
constexpr std::size_t kPrimaryIndexCapacity = 27;
/** The second trailing index bank holds 5 definition indices. */
constexpr std::size_t kSecondaryIndexCapacity = 5;
/** 7 tail bytes complete the native object stride after its final flag. */
constexpr std::size_t kTailSize = 7;
/** A missing biased 16-bit definition index is every bit set. */
constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();

/** Native family-3 roster object with explicit ABI padding and sizes. */
struct RosterLayout {
    std::uint64_t accountSoid{};
    roster::Block roster{};
    std::array<std::uint16_t, kPrimaryIndexCapacity> primaryIndices{};
    std::array<std::uint16_t, kSecondaryIndexCapacity> secondaryIndices{};
    std::uint8_t flag{};
    std::array<std::byte, kTailSize> tail{};
};

static_assert(sizeof(RosterLayout) == kRosterSize);
static_assert(offsetof(RosterLayout, primaryIndices)
              == sizeof(std::uint64_t) + sizeof(roster::Block));
static_assert(offsetof(RosterLayout, secondaryIndices)
              == offsetof(RosterLayout, primaryIndices)
                     + kPrimaryIndexCapacity * sizeof(std::uint16_t));
static_assert(offsetof(RosterLayout, flag)
              == offsetof(RosterLayout, secondaryIndices)
                     + kSecondaryIndexCapacity * sizeof(std::uint16_t));

} // namespace

/** Encodes a sentinel-correct account roster from authored State identities. */
bool encode_roster(const state::AccountState& account,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (account.primarySoid == 0 || output.size() < sizeof(RosterLayout)) {
        return false;
    }

    RosterLayout layout;
    layout.accountSoid = account.primarySoid;
    if (!roster::initialize(account, layout.roster)) {
        return false;
    }
    std::fill(layout.primaryIndices.begin(), layout.primaryIndices.end(), kEmptyDefinitionIndex);
    std::fill(
        layout.secondaryIndices.begin(), layout.secondaryIndices.end(), kEmptyDefinitionIndex);
    std::memcpy(output.data(), &layout, sizeof layout);
    written = sizeof layout;
    return true;
}

} // namespace sunrise::middleware::datagen::family3
