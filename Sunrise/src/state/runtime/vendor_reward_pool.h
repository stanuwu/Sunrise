#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../account/account_state.h"

namespace sunrise::state::vendor_rewards {

/** Current Vanguard package item hash in build 86657. */
inline constexpr std::uint32_t kVanguardPackageHash = 2746484552U;
/** Current Crucible package item hash in build 86657. */
inline constexpr std::uint32_t kCruciblePackageHash = 3289621657U;
/** Current Gunsmith package item hash in build 86657. */
inline constexpr std::uint32_t kGunsmithPackageHash = 2422825785U;
/** Only these three packages have a supported one-gear payout policy. */
inline constexpr std::array<std::uint32_t, 3> kPackageHashes{
    kVanguardPackageHash, kCruciblePackageHash, kGunsmithPackageHash};

/** The largest build-86657 class list has 30 weapons and five armour pieces. */
inline constexpr std::size_t kCandidateCapacity = 35;

/** Installed candidate order for each character class; no item hashes are compiled here. */
struct Pool {
    std::uint32_t packageHash{};
    std::array<std::array<std::uint32_t, kCandidateCapacity>, 3> items{};
    std::array<std::size_t, 3> counts{};
};

/** @return True after the complete supported package set has been published. */
[[nodiscard]] bool ready() noexcept;

/** @return True after extraction either published the pools or ruled them unavailable. */
[[nodiscard]] bool settled() noexcept;

/** Records that this installed build has no readable supported pool; claims still fail closed. */
void settle_unavailable() noexcept;

/**
 * Replaces the supported package pools together, refusing malformed or duplicate candidates.
 * @param pools One pool for each supported installed package.
 * @return True when all pools are valid and published.
 */
[[nodiscard]] bool replace(std::span<const Pool> pools) noexcept;

/**
 * @param packageHash Installed package item hash.
 * @param output Receives its pool on success.
 * @return True only for a published supported package.
 */
[[nodiscard]] bool find(std::uint32_t packageHash, Pool& output) noexcept;

/** @return Candidates for a valid selected class, or empty for an invalid class. */
[[nodiscard]] std::span<const std::uint32_t> candidates(const Pool& pool,
                                                        CharacterClass characterClass) noexcept;

} // namespace sunrise::state::vendor_rewards
