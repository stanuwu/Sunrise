/**
 * The mission variable change history. The server thread writes it from the VM's commit; the
 * HUD reads it on the presentation thread. One lock guards the ring.
 */

#include "mission_script_variable_log.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>

namespace sunrise::server::activity::mission::variable_log {
namespace {

namespace mission_state = state::activity::mission;

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Entry, kEntryCapacity> g_entries{};
/** Ring index the next entry is written to. */
std::size_t g_next{};
std::size_t g_count{};
/** Written under the lock, read without it by `newest_sequence`. */
std::atomic<std::uint64_t> g_newestSequence{};

/** @return The key's bytes as a bounded view. */
[[nodiscard]] std::string_view key_view(const mission_state::StateKey& key) noexcept {
    return {key.bytes.data(), (std::min)(static_cast<std::size_t>(key.length), key.bytes.size())};
}

/** @return True when both values are the same kind and hold the same scalar. */
[[nodiscard]] bool same_value(const mission_state::VariableValue& left,
                              const mission_state::VariableValue& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
    case mission_state::VariableValueKind::boolean:
        return left.booleanValue == right.booleanValue;
    case mission_state::VariableValueKind::integer:
        return left.integerValue == right.integerValue;
    case mission_state::VariableValueKind::real:
        return left.realValue == right.realValue;
    case mission_state::VariableValueKind::string:
        return left.stringLength == right.stringLength
               && std::equal(left.stringValue.begin(),
                             left.stringValue.begin()
                                 + (std::min)(static_cast<std::size_t>(left.stringLength),
                                              left.stringValue.size()),
                             right.stringValue.begin());
    }
    return false;
}

/** Appends one change, holding the exclusive lock the caller already took. */
void append_locked(std::string_view activityId,
                   const mission_state::StateKey& key,
                   const mission_state::VariableValue* previous,
                   const mission_state::VariableValue* value,
                   Change change,
                   std::uint64_t tick) noexcept {
    Entry& entry = g_entries[g_next];
    entry = {};
    const std::size_t idLength = (std::min)(activityId.size(), entry.activityId.size() - 1);
    std::copy_n(activityId.data(), idLength, entry.activityId.data());
    entry.key = key;
    if (value != nullptr) {
        entry.value = *value;
    }
    if (previous != nullptr) {
        entry.previous = *previous;
    }
    entry.change = change;
    entry.tick = tick;
    entry.sequence = g_newestSequence.load(std::memory_order_relaxed) + 1;
    g_newestSequence.store(entry.sequence, std::memory_order_release);
    g_next = (g_next + 1) % kEntryCapacity;
    g_count = (std::min)(g_count + 1, kEntryCapacity);
}

} // namespace

/** Merges the two sorted sets and records each key that was added, changed or cleared. */
void record_commit(std::string_view activityId,
                   std::span<const mission_state::ScriptVariable> before,
                   std::span<const mission_state::ScriptVariable> after) noexcept {
    // The lock is taken at the first difference, so a callback that only moved a timer or the
    // phase costs no lock at all.
    bool locked = false;
    std::uint64_t tick = 0;
    const auto lock = [&]() noexcept {
        if (!locked) {
            AcquireSRWLockExclusive(&g_lock);
            tick = GetTickCount64();
            locked = true;
        }
    };
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
    while (oldIndex < before.size() || newIndex < after.size()) {
        if (newIndex == after.size()
            || (oldIndex < before.size()
                && key_view(before[oldIndex].key) < key_view(after[newIndex].key))) {
            lock();
            append_locked(activityId,
                          before[oldIndex].key,
                          &before[oldIndex].value,
                          nullptr,
                          Change::cleared,
                          tick);
            ++oldIndex;
            continue;
        }
        if (oldIndex == before.size()
            || key_view(after[newIndex].key) < key_view(before[oldIndex].key)) {
            lock();
            append_locked(activityId,
                          after[newIndex].key,
                          nullptr,
                          &after[newIndex].value,
                          Change::added,
                          tick);
            ++newIndex;
            continue;
        }
        if (!same_value(before[oldIndex].value, after[newIndex].value)) {
            lock();
            append_locked(activityId,
                          after[newIndex].key,
                          &before[oldIndex].value,
                          &after[newIndex].value,
                          Change::changed,
                          tick);
        }
        ++oldIndex;
        ++newIndex;
    }
    if (locked) {
        ReleaseSRWLockExclusive(&g_lock);
    }
}

/** @return Sequence of the newest entry. */
std::uint64_t newest_sequence() noexcept {
    return g_newestSequence.load(std::memory_order_acquire);
}

/** Copies every retained change, oldest first. */
void snapshot(Snapshot& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    output.count = g_count;
    output.newestSequence = g_newestSequence.load(std::memory_order_relaxed);
    const std::size_t first = (g_next + kEntryCapacity - g_count) % kEntryCapacity;
    for (std::size_t index = 0; index < g_count; ++index) {
        output.entries[index] = g_entries[(first + index) % kEntryCapacity];
    }
    ReleaseSRWLockShared(&g_lock);
}

} // namespace sunrise::server::activity::mission::variable_log
