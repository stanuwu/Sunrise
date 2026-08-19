#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../world/host_command.h"

namespace sunrise::server::gameplay::physics::persistence {

/** The process journal retains bounded work until a checkpoint advances its durable floor. */
inline constexpr std::size_t kJournalRecordCapacity = 4'096;
/** The journal tracks the same number of logical worlds as the checkpoint store. */
inline constexpr std::size_t kJournalWorldCapacity = 4;

/** One durable command and its monotonic logical-world cursor. */
struct JournalRecord final {
    world::HostCommand command{};
    std::uint64_t logicalWorldId{};
    std::uint64_t cursor{};
};

/** Bounded results for journal append, read, and floor advancement. */
enum class JournalResult : std::uint8_t {
    appended,
    empty,
    read,
    discarded,
    invalid,
    notFound,
    staleCursor,
    gap,
    capacity,
    exhausted,
};

/** Fixed command journal with caller-controlled checkpoint floors. */
class CommandJournal final {
public:
    /** Clears records and logical-world cursors. */
    void reset() noexcept;

    /** Appends one exact committed batch after the caller's expected tail. */
    [[nodiscard]] JournalResult append(std::uint64_t logicalWorldId,
                                       std::uint64_t expectedCursor,
                                       std::span<const world::HostCommand> commands,
                                       std::uint64_t& tailCursor) noexcept;
    /** Copies retained records after one exact cursor without changing output on failure. */
    [[nodiscard]] JournalResult read_after(std::uint64_t logicalWorldId,
                                           std::uint64_t afterCursor,
                                           std::span<JournalRecord> records,
                                           std::size_t& recordCount,
                                           std::uint64_t& tailCursor) const noexcept;
    /** Copies replay commands and replaces only their process-local world identity. */
    [[nodiscard]] JournalResult rebind_after(std::uint64_t logicalWorldId,
                                             std::uint64_t afterCursor,
                                             const world::WorldIdentity& worldIdentity,
                                             std::span<world::HostCommand> commands,
                                             std::size_t& commandCount,
                                             std::uint64_t& tailCursor) const noexcept;
    /** Copies one bounded replay page and returns the cursor covered by that page. */
    [[nodiscard]] JournalResult rebind_page_after(std::uint64_t logicalWorldId,
                                                  std::uint64_t afterCursor,
                                                  const world::WorldIdentity& worldIdentity,
                                                  std::span<world::HostCommand> commands,
                                                  std::size_t& commandCount,
                                                  std::uint64_t& pageCursor,
                                                  std::uint64_t& tailCursor) const noexcept;
    /** Discards records covered by a durable checkpoint while retaining the monotonic tail. */
    [[nodiscard]] JournalResult discard_through(std::uint64_t logicalWorldId,
                                                std::uint64_t cursor) noexcept;
    /** Copies one logical world's tail and durable floor. */
    [[nodiscard]] bool cursors(std::uint64_t logicalWorldId,
                               std::uint64_t& retainedFloor,
                               std::uint64_t& tailCursor) const noexcept;
    /** @return Number of retained command records across all worlds. */
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct WorldCursor final {
        std::uint64_t logicalWorldId{};
        std::uint64_t retainedFloor{};
        std::uint64_t tail{};
        bool occupied{};
    };

    std::array<JournalRecord, kJournalRecordCapacity> records_{};
    std::array<WorldCursor, kJournalWorldCapacity> worlds_{};
    std::size_t recordCount_{};
};

} // namespace sunrise::server::gameplay::physics::persistence
