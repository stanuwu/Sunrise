#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Fixed combat storage and coordinate limits bound deterministic kernel work. */
inline constexpr std::size_t kCombatantCapacity = 64;
inline constexpr std::size_t kAttackBlueprintCapacity = 32;
inline constexpr std::size_t kCombatRewindSampleCapacity = 16;
inline constexpr std::int32_t kCombatCoordinateLimitMillimeters = 1'000'000'000;
inline constexpr std::uint32_t kCombatRangeLimitMillimeters = 1'000'000'000;
/** The invalid combatant slot lies immediately outside fixed combatant storage. */
inline constexpr std::uint16_t kInvalidCombatantSlot =
    static_cast<std::uint16_t>(kCombatantCapacity);
/** The invalid attack slot lies immediately outside fixed blueprint storage. */
inline constexpr std::uint16_t kInvalidAttackBlueprintSlot =
    static_cast<std::uint16_t>(kAttackBlueprintCapacity);

struct CombatantHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidCombatantSlot};
};

struct AttackBlueprintHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidAttackBlueprintSlot};
};

enum class CombatResult : std::uint8_t {
    applied,
    rejected,
    duplicate,
    noChange,
    invalidDefinition,
    invalidHandle,
    invalidTick,
    capacity,
    outputCapacity,
};

enum class CombatRejectReason : std::uint8_t {
    none,
    invalidCommand,
    staleWorld,
    invalidSource,
    invalidTarget,
    invalidAttack,
    policyOwner,
    sourceDead,
    targetDead,
    targetAlive,
    combatDisabled,
    notHostile,
    cooldown,
    claimedTick,
    rewindMissing,
    authority,
    range,
    lineOfSight,
    duplicate,
    capacity,
};

enum class CombatEventKind : std::uint8_t {
    damageAccepted,
    damageRejected,
    respawnRejected,
    healthChanged,
    death,
    respawn,
};

struct CombatantDefinition final {
    ActorHandle actor{};
    PolicyOwnerId policyOwnerId{};
    std::uint64_t hostileFactionMask{};
    std::uint32_t bubble{};
    std::uint32_t maximumHealth{};
    std::uint32_t maximumShield{};
    std::uint8_t faction{};
    bool canDealDamage{};
    bool canReceiveDamage{};
};

struct AttackBlueprintDefinition final {
    std::uint64_t attackBlueprintId{};
    PolicyOwnerId policyOwnerId{};
    std::uint32_t blueprintVersion{};
    std::uint32_t damage{};
    std::uint32_t maximumRangeMillimeters{};
    TickId cooldownTicks{};
};

struct CombatPoseSample final {
    PositionMillimeters position{};
    TickId tick{};
    std::uint64_t authorityEpoch{};
};

/** Evidence must come from the host collision and rewind adapters. */
struct HostHitEvidence final {
    std::uint64_t sourceAuthorityEpoch{};
    bool lineOfSight{};
};

struct DamageIntent final {
    CommandHeader command{};
    ActorHandle sourceActor{};
    ActorHandle targetActor{};
    AttackBlueprintHandle attackBlueprint{};
    TickId claimedTick{};
    HostHitEvidence hitEvidence{};
};

struct RespawnIntent final {
    CommandHeader command{};
    ActorHandle targetActor{};
};

struct CombatEvent final {
    ActorHandle sourceActor{};
    ActorHandle targetActor{};
    WorldEpoch worldEpoch{};
    TickId tick{};
    CommandId commandId{};
    std::uint64_t attackBlueprintId{};
    std::uint64_t sourceRevision{};
    std::uint64_t targetRevision{};
    std::uint32_t oldHealth{};
    std::uint32_t newHealth{};
    std::uint32_t oldShield{};
    std::uint32_t newShield{};
    CombatEventKind kind{CombatEventKind::damageRejected};
    CombatRejectReason rejectReason{CombatRejectReason::none};
};

struct CombatantSnapshot final {
    CombatantDefinition definition{};
    std::array<CombatPoseSample, kCombatRewindSampleCapacity> rewindSamples{};
    std::uint64_t generation{};
    std::uint64_t revision{};
    std::uint64_t authorityEpoch{};
    TickId nextDamageTick{};
    std::size_t rewindSampleCount{};
    std::uint32_t health{};
    std::uint32_t shield{};
    bool active{};
    bool alive{};
};

struct AttackBlueprintSnapshot final {
    AttackBlueprintDefinition definition{};
    std::uint64_t generation{};
    bool active{};
};

struct CombatKernelSnapshot final {
    std::array<CombatantSnapshot, kCombatantCapacity> combatants{};
    std::array<AttackBlueprintSnapshot, kAttackBlueprintCapacity> attacks{};
    CommandDeduplicatorSnapshot commands{};
    WorldEpoch worldEpoch{};
};

/** Deterministic health, shield, death, and respawn mutation. */
class CombatKernel final {
public:
    /** Clears all combat state and keeps combat inert until configured. */
    void reset(WorldEpoch worldEpoch) noexcept;

    /** Registers one actor. Default flags and hostility reject all damage. */
    [[nodiscard]] CombatResult create_combatant(const CombatantDefinition& definition,
                                                CombatantHandle& handle) noexcept;

    /** Removes one exact combatant generation. */
    [[nodiscard]] CombatResult remove_combatant(CombatantHandle handle) noexcept;

    /** Registers one versioned attack blueprint. */
    [[nodiscard]] CombatResult create_attack_blueprint(const AttackBlueprintDefinition& definition,
                                                       AttackBlueprintHandle& handle) noexcept;

    /** Removes one exact attack blueprint generation. */
    [[nodiscard]] CombatResult remove_attack_blueprint(AttackBlueprintHandle handle) noexcept;

    /** Changes motion authority and clears incompatible rewind samples. */
    [[nodiscard]] CombatResult set_authority(CombatantHandle handle,
                                             std::uint64_t authorityEpoch) noexcept;

    /** Retains one bounded canonical pose for later hit validation. */
    [[nodiscard]] CombatResult record_pose(CombatantHandle handle,
                                           const CombatPoseSample& sample) noexcept;

    /** Validates and commits one idempotent damage intent. */
    [[nodiscard]] CombatResult submit_damage(const DamageIntent& intent,
                                             std::span<CombatEvent> events,
                                             std::size_t& eventCount) noexcept;

    /** Restores one dead actor to its configured full health and shield. */
    [[nodiscard]] CombatResult respawn(const RespawnIntent& intent,
                                       std::span<CombatEvent> events,
                                       std::size_t& eventCount) noexcept;

    /** Releases command ids older than the durable replay floor. */
    void expire_commands_before(TickId firstRetainedTick) noexcept;

    /** Copies one combatant without exposing mutable storage. */
    [[nodiscard]] bool query(CombatantHandle handle, CombatantSnapshot& combatant) const noexcept;

    /** Copies fixed combat and command state. */
    [[nodiscard]] CombatKernelSnapshot snapshot() const noexcept;

    /** Restores a validated fixed snapshot. */
    [[nodiscard]] bool restore(const CombatKernelSnapshot& snapshot) noexcept;

private:
    [[nodiscard]] bool valid_combatant_handle(CombatantHandle handle) const noexcept;
    [[nodiscard]] bool valid_attack_handle(AttackBlueprintHandle handle) const noexcept;
    [[nodiscard]] std::size_t find_actor(ActorHandle actor) const noexcept;

    std::array<CombatantSnapshot, kCombatantCapacity> combatants_{};
    std::array<AttackBlueprintSnapshot, kAttackBlueprintCapacity> attacks_{};
    CommandDeduplicator commands_{};
    WorldEpoch worldEpoch_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
