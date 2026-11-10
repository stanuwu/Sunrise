#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "../unlocks/unlocks_expression.h"
#include "pursuit_completion_runtime.h"

namespace sunrise::state {

/** Why no snapshot was produced. A partial snapshot is never returned. */
enum class PursuitSnapshotRefusal : std::uint8_t {
    none,
    /** The objective or pursuit catalog is not published, or was republished mid-read. */
    catalogUnavailable,
    /** The installed data has settled that no pursuit catalog can be read. */
    catalogUnsupported,
    /** The saved account could not be read or is invalid. */
    accountUnavailable,
    /** No character is selected. */
    characterUnavailable,
    /** A SQLite read or an allocation failed. */
    storage,
};

/** One unequipped pursuit-bucket resident and its saved-bank evaluation. */
struct HeldPursuitStatus {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t mutationSerial{};
    std::uint32_t flags{};
    /** A positive lifetime marks a bounty. */
    std::int32_t lifetimeSeconds{};
    PursuitCompletionStatus completion{};
};

/** One coherent saved view of a character's held pursuits. */
struct PursuitSnapshot {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::vector<HeldPursuitStatus> held;
};

/**
 * Reads the selected character's saved inventory and unlock banks under one store lock, then
 * evaluates installed contracts against those copies. It never writes, draws or publishes.
 * @param output Receives the snapshot, and is cleared on any refusal.
 * @return None on success, otherwise why no snapshot was produced.
 */
[[nodiscard]] PursuitSnapshotRefusal read_pursuit_snapshot(PursuitSnapshot& output) noexcept;

/** One saved flag or value row. A flag reads 0 or 1. */
struct UnlockRead {
    bool known{};
    std::int32_t value{};
};

/** The saved account and character one unlock read came from. */
struct UnlockReadOrigin {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
};

/**
 * Reads any of the selected character's saved flag and value rows under one store lock; the rows
 * need not belong to a pursuit. It never writes, draws or publishes.
 * @param sources One flag or value instruction per row. A source the banks cannot answer reads as
 * unknown.
 * @param output Receives one read per source, in order. It must be at least as long as sources.
 * @param origin Receives the saved identity the reads came from.
 * @return None on success, otherwise why nothing was read.
 */
[[nodiscard]] PursuitSnapshotRefusal
read_unlock_values(std::span<const unlocks::Instruction> sources,
                   std::span<UnlockRead> output,
                   UnlockReadOrigin& origin) noexcept;

} // namespace sunrise::state
