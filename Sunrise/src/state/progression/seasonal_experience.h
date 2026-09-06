#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state {
struct Family5State;
}

namespace sunrise::state::progression::seasonal_experience {

inline constexpr std::uint16_t kArtifactPowerProgressionDefinitionIndex = 38;
inline constexpr std::uint16_t kArtifactUnlockProgressionDefinitionIndex = 39;
inline constexpr std::uint16_t kArtifactSaleCount = 26;
inline constexpr std::int32_t kExperiencePerRank = 100'000;
inline constexpr std::uint16_t kMaximumRank = 100;
inline constexpr std::int32_t kMaximumPassExperience =
    (static_cast<std::int32_t>(kMaximumRank) - 1) * kExperiencePerRank;
inline constexpr std::uint32_t kSeedOfSilverWingsHash = 0x613A3DA6U;

/** Resolves persistent storage and restores earned seasonal XP. */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Clears process memory without deleting persisted XP. */
void shutdown() noexcept;

/** Adds unmodified base XP to the account's seasonal progression. */
[[nodiscard]] bool grant(std::int32_t amount) noexcept;

/** Returns runtime-earned seasonal XP. */
[[nodiscard]] std::int32_t earned() noexcept;

/** Returns the one-based Season of Arrivals rank earned by the persisted XP total. */
[[nodiscard]] std::uint16_t rank() noexcept;

/** Returns the account-wide Power bonus earned by the seasonal artifact. */
[[nodiscard]] std::uint16_t artifact_power_bonus() noexcept;

/** Returns whether one native Season of Arrivals reward row was already claimed. */
[[nodiscard]] bool reward_claimed(std::uint16_t rewardIndex) noexcept;

/** Persistently claims one native reward row exactly once. */
[[nodiscard]] bool claim_reward(std::uint16_t rewardIndex) noexcept;

/** Publishes persisted Season Pass claims into their mapped account acquired-flag bytes. */
[[nodiscard]] bool apply_reward_claims(std::span<std::uint8_t> acquiredFlags) noexcept;

/** Validates one artifact vendor row and returns its exact uncommitted mask transition. */
[[nodiscard]] bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                               std::uint32_t& expected,
                                               std::uint32_t& replacement) noexcept;

/** Returns the persisted artifact purchase mask. */
[[nodiscard]] std::uint32_t artifact_mod_mask() noexcept;

/** Replaces the exact persisted mask, refusing if another action changed it first. */
[[nodiscard]] bool replace_artifact_mod_mask(std::uint32_t expected,
                                             std::uint32_t replacement) noexcept;

/** Projects artifact counters and removes flags carried by the native character bank. */
[[nodiscard]] bool apply_artifact_state(Family5State& family) noexcept;

/** Projects artifact ownership and spent points into the live Family-4 character object. */
[[nodiscard]] bool apply_artifact_character_state(std::span<std::byte> acquiredFlags,
                                                  std::span<std::int32_t> objectiveValues) noexcept;

/** Projects an explicit uncommitted artifact mask into a Family-4 character object. */
[[nodiscard]] bool apply_artifact_character_state(std::uint32_t artifactMask,
                                                  std::span<std::byte> acquiredFlags,
                                                  std::span<std::int32_t> objectiveValues) noexcept;

} // namespace sunrise::state::progression::seasonal_experience
