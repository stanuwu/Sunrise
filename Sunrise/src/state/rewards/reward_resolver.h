#pragma once

#include <bitset>

#include "../account/account_state.h"
#include "../build_data/rewards/definition.h"
#include "../unlocks/definition.h"

namespace sunrise::state::reward_resolution {

/** One item a resolved reward grants, with the socket overrides its row carries. */
struct Grant {
    std::uint16_t itemIndex{build_data::rewards::kAbsent};
    std::int32_t quantity{};
    std::array<build_data::rewards::SocketOverride, build_data::rewards::kSocketsPerItem> sockets{};
    std::size_t socketCount{};
};

/** Every grant one resolution produces, in draw order. */
struct Result {
    std::array<Grant, build_data::rewards::kGrantCapacity> grants{};
    std::size_t count{};
    /** Consumed wrappers acquire flags without occupying inventory grant rows. */
    std::bitset<unlocks::kAccountFlagCapacity> wrapperFlags{};
};

/** The seed belongs to the prepared server transaction, so validation repeats the same draw. */
struct Context {
    const unlocks::Table& unlocks;
    CharacterClass characterClass{};
    std::uint64_t seed{};
    /** Optional refusal reason; points to a static diagnostic label. */
    const char** refusal{};
};

/** Evaluates one bound condition; an empty condition is always eligible. */
[[nodiscard]] bool eligible(std::span<const build_data::rewards::Instruction> instructions,
                            const Context& context,
                            bool& result) noexcept;

/** How one resolution ended; ineligible means the saved unlocks left nothing to draw. */
enum class Resolution : std::uint8_t { resolved, ineligible, refused };

/** Plans an acquisition without changing State; retains wrappers opened separately. */
[[nodiscard]] Resolution resolve(const Context& context,
                                 std::uint16_t itemIndex,
                                 std::uint32_t quantity,
                                 Result& result) noexcept;

} // namespace sunrise::state::reward_resolution
