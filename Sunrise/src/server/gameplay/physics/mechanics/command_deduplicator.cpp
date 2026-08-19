#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

void CommandDeduplicator::clear() noexcept {
    records_ = {};
    count_ = 0;
}

/** @return True when one policy owner already committed the command id. */
bool CommandDeduplicator::contains(PolicyOwnerId policyOwnerId,
                                   CommandId commandId) const noexcept {
    if (policyOwnerId == kAbsentPolicyOwnerId || commandId == kAbsentCommandId) {
        return false;
    }
    for (const CommandRecord& record : records_) {
        if (record.occupied && record.policyOwnerId == policyOwnerId
            && record.commandId == commandId) {
            return true;
        }
    }
    return false;
}

/** Records one command without evicting a retained replay guard. */
DeduplicationResult CommandDeduplicator::remember(const CommandHeader& header) noexcept {
    if (header.policyOwnerId == kAbsentPolicyOwnerId || header.commandId == kAbsentCommandId) {
        return DeduplicationResult::invalid;
    }
    if (contains(header.policyOwnerId, header.commandId)) {
        return DeduplicationResult::duplicate;
    }
    for (CommandRecord& record : records_) {
        if (!record.occupied) {
            record =
                CommandRecord{header.commandId, header.executionTick, header.policyOwnerId, true};
            ++count_;
            return DeduplicationResult::recorded;
        }
    }
    return DeduplicationResult::capacity;
}

void CommandDeduplicator::expire_before(TickId firstRetainedTick) noexcept {
    for (CommandRecord& record : records_) {
        if (record.occupied && record.executionTick < firstRetainedTick) {
            record = {};
            --count_;
        }
    }
}

std::size_t CommandDeduplicator::size() const noexcept {
    return count_;
}

CommandDeduplicatorSnapshot CommandDeduplicator::snapshot() const noexcept {
    return CommandDeduplicatorSnapshot{records_, count_};
}

/** Replaces command history only after count and uniqueness validation. */
bool CommandDeduplicator::restore(const CommandDeduplicatorSnapshot& snapshot) noexcept {
    if (snapshot.count > snapshot.records.size()) {
        return false;
    }
    std::size_t occupied = 0;
    for (std::size_t left = 0; left < snapshot.records.size(); ++left) {
        const CommandRecord& record = snapshot.records[left];
        if (!record.occupied) {
            if (record.commandId != kAbsentCommandId || record.policyOwnerId != kAbsentPolicyOwnerId
                || record.executionTick != 0) {
                return false;
            }
            continue;
        }
        if (record.commandId == kAbsentCommandId || record.policyOwnerId == kAbsentPolicyOwnerId) {
            return false;
        }
        ++occupied;
        for (std::size_t right = left + 1; right < snapshot.records.size(); ++right) {
            const CommandRecord& other = snapshot.records[right];
            if (other.occupied && record.commandId == other.commandId
                && record.policyOwnerId == other.policyOwnerId) {
                return false;
            }
        }
    }
    if (occupied != snapshot.count) {
        return false;
    }
    records_ = snapshot.records;
    count_ = occupied;
    return true;
}

} // namespace sunrise::server::gameplay::physics::mechanics
