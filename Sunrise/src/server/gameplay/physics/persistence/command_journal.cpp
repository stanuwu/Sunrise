#include "command_journal.h"

#include <algorithm>
#include <limits>

namespace sunrise::server::gameplay::physics::persistence {
namespace {

/** @return One logical-world cursor slot, or the fixed invalid sentinel. */
template <typename WorldArray>
[[nodiscard]] std::size_t find_world(const WorldArray& worlds,
                                     std::uint64_t logicalWorldId) noexcept {
    for (std::size_t index = 0; index < worlds.size(); ++index) {
        if (worlds[index].occupied && worlds[index].logicalWorldId == logicalWorldId) {
            return index;
        }
    }
    return worlds.size();
}

/** @return True when one committed batch has stable identity and order. */
[[nodiscard]] bool valid_batch(std::span<const world::HostCommand> commands) noexcept {
    if (commands.empty()) {
        return true;
    }
    const world::WorldIdentity identity = commands.front().header.world;
    if (!world::valid_world(identity)) {
        return false;
    }
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const world::HostCommand& command = commands[index];
        if (!world::valid_command_header(command.header)
            || !world::equal_world(command.header.world, identity)
            || world::command_kind(command) >= world::HostCommandKind::count
            || (index != 0
                && commands[index - 1].header.executeTick > command.header.executeTick)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears records and logical-world cursors. */
void CommandJournal::reset() noexcept {
    records_ = {};
    worlds_ = {};
    recordCount_ = 0;
}

/** Appends one exact committed batch after the caller's expected tail. */
JournalResult CommandJournal::append(std::uint64_t logicalWorldId,
                                     std::uint64_t expectedCursor,
                                     std::span<const world::HostCommand> commands,
                                     std::uint64_t& tailCursor) noexcept {
    tailCursor = 0;
    if (logicalWorldId == 0 || !valid_batch(commands)) {
        return JournalResult::invalid;
    }
    std::size_t worldSlot = find_world(worlds_, logicalWorldId);
    if (worldSlot == worlds_.size()) {
        if (expectedCursor != 0) {
            return JournalResult::notFound;
        }
        for (std::size_t index = 0; index < worlds_.size(); ++index) {
            if (!worlds_[index].occupied) {
                worldSlot = index;
                break;
            }
        }
        if (worldSlot == worlds_.size()) {
            return JournalResult::capacity;
        }
    } else if (worlds_[worldSlot].tail != expectedCursor) {
        return JournalResult::staleCursor;
    }
    if (commands.empty()) {
        tailCursor = expectedCursor;
        return JournalResult::empty;
    }
    if (commands.size() > records_.size() - recordCount_) {
        return JournalResult::capacity;
    }
    if (expectedCursor > (std::numeric_limits<std::uint64_t>::max)() - commands.size()) {
        return JournalResult::exhausted;
    }

    WorldCursor& cursor = worlds_[worldSlot];
    if (!cursor.occupied) {
        cursor.logicalWorldId = logicalWorldId;
        cursor.occupied = true;
    }
    for (const world::HostCommand& command : commands) {
        JournalRecord& record = records_[recordCount_++];
        record.command = command;
        record.logicalWorldId = logicalWorldId;
        record.cursor = ++cursor.tail;
    }
    tailCursor = cursor.tail;
    return JournalResult::appended;
}

/** Copies retained records after one exact cursor. */
JournalResult CommandJournal::read_after(std::uint64_t logicalWorldId,
                                         std::uint64_t afterCursor,
                                         std::span<JournalRecord> records,
                                         std::size_t& recordCount,
                                         std::uint64_t& tailCursor) const noexcept {
    recordCount = 0;
    tailCursor = 0;
    const std::size_t worldSlot = find_world(worlds_, logicalWorldId);
    if (logicalWorldId == 0 || worldSlot == worlds_.size()) {
        return JournalResult::notFound;
    }
    const WorldCursor& cursor = worlds_[worldSlot];
    if (afterCursor < cursor.retainedFloor) {
        return JournalResult::gap;
    }
    if (afterCursor > cursor.tail) {
        return JournalResult::staleCursor;
    }
    std::size_t required = 0;
    for (std::size_t index = 0; index < recordCount_; ++index) {
        required +=
            records_[index].logicalWorldId == logicalWorldId && records_[index].cursor > afterCursor
                ? 1U
                : 0U;
    }
    if (required > records.size()) {
        return JournalResult::capacity;
    }
    for (std::size_t index = 0; index < recordCount_; ++index) {
        if (records_[index].logicalWorldId == logicalWorldId
            && records_[index].cursor > afterCursor) {
            records[recordCount++] = records_[index];
        }
    }
    tailCursor = cursor.tail;
    return required == 0 ? JournalResult::empty : JournalResult::read;
}

/** Copies retained replay commands under one fresh process-local world identity. */
JournalResult CommandJournal::rebind_after(std::uint64_t logicalWorldId,
                                           std::uint64_t afterCursor,
                                           const world::WorldIdentity& worldIdentity,
                                           std::span<world::HostCommand> commands,
                                           std::size_t& commandCount,
                                           std::uint64_t& tailCursor) const noexcept {
    commandCount = 0;
    tailCursor = 0;
    if (!world::valid_world(worldIdentity)) {
        return JournalResult::invalid;
    }
    std::array<JournalRecord, world::kCommandQueueCapacity> retained{};
    std::size_t retainedCount = 0;
    const JournalResult result =
        read_after(logicalWorldId, afterCursor, retained, retainedCount, tailCursor);
    if (result != JournalResult::read && result != JournalResult::empty) {
        return result;
    }
    if (retainedCount > commands.size()) {
        return JournalResult::capacity;
    }
    for (std::size_t index = 0; index < retainedCount; ++index) {
        commands[index] = retained[index].command;
        commands[index].header.world = worldIdentity;
    }
    commandCount = retainedCount;
    return result;
}

/** Copies one bounded replay page under a fresh process-local world identity. */
JournalResult CommandJournal::rebind_page_after(std::uint64_t logicalWorldId,
                                                std::uint64_t afterCursor,
                                                const world::WorldIdentity& worldIdentity,
                                                std::span<world::HostCommand> commands,
                                                std::size_t& commandCount,
                                                std::uint64_t& pageCursor,
                                                std::uint64_t& tailCursor) const noexcept {
    commandCount = 0;
    pageCursor = 0;
    tailCursor = 0;
    if (!world::valid_world(worldIdentity) || commands.empty()) {
        return JournalResult::invalid;
    }
    const std::size_t worldSlot = find_world(worlds_, logicalWorldId);
    if (logicalWorldId == 0 || worldSlot == worlds_.size()) {
        return JournalResult::notFound;
    }
    const WorldCursor& cursor = worlds_[worldSlot];
    if (afterCursor < cursor.retainedFloor) {
        return JournalResult::gap;
    }
    if (afterCursor > cursor.tail) {
        return JournalResult::staleCursor;
    }
    for (std::size_t index = 0; index < recordCount_ && commandCount < commands.size(); ++index) {
        const JournalRecord& record = records_[index];
        if (record.logicalWorldId != logicalWorldId || record.cursor <= afterCursor) {
            continue;
        }
        commands[commandCount] = record.command;
        commands[commandCount].header.world = worldIdentity;
        ++commandCount;
        pageCursor = record.cursor;
    }
    tailCursor = cursor.tail;
    if (commandCount == 0) {
        pageCursor = afterCursor;
        return JournalResult::empty;
    }
    return JournalResult::read;
}

/** Discards records covered by one durable checkpoint. */
JournalResult CommandJournal::discard_through(std::uint64_t logicalWorldId,
                                              std::uint64_t discardCursor) noexcept {
    const std::size_t worldSlot = find_world(worlds_, logicalWorldId);
    if (logicalWorldId == 0 || worldSlot == worlds_.size()) {
        return JournalResult::notFound;
    }
    WorldCursor& cursor = worlds_[worldSlot];
    if (discardCursor < cursor.retainedFloor || discardCursor > cursor.tail) {
        return JournalResult::staleCursor;
    }
    std::size_t retained = 0;
    for (std::size_t index = 0; index < recordCount_; ++index) {
        const JournalRecord& record = records_[index];
        if (record.logicalWorldId != logicalWorldId || record.cursor > discardCursor) {
            records_[retained++] = record;
        }
    }
    for (std::size_t index = retained; index < recordCount_; ++index) {
        records_[index] = {};
    }
    recordCount_ = retained;
    cursor.retainedFloor = discardCursor;
    return JournalResult::discarded;
}

/** Copies one logical world's durable floor and tail. */
bool CommandJournal::cursors(std::uint64_t logicalWorldId,
                             std::uint64_t& retainedFloor,
                             std::uint64_t& tailCursor) const noexcept {
    retainedFloor = 0;
    tailCursor = 0;
    const std::size_t worldSlot = find_world(worlds_, logicalWorldId);
    if (worldSlot == worlds_.size()) {
        return false;
    }
    retainedFloor = worlds_[worldSlot].retainedFloor;
    tailCursor = worlds_[worldSlot].tail;
    return true;
}

/** Returns the retained record count. */
std::size_t CommandJournal::size() const noexcept {
    return recordCount_;
}

} // namespace sunrise::server::gameplay::physics::persistence
