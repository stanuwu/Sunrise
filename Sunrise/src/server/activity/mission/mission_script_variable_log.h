#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../state/activity/mission/definition.h"

/**
 * A short history of the mission variables scripts changed, kept for the HUD.
 * The VM records each committed callback's changes here. Nothing reads it back into a script, so
 * it is display only and never durable.
 */
namespace sunrise::server::activity::mission::variable_log {

/** Changes retained. The HUD shows the newest of them. */
inline constexpr std::size_t kEntryCapacity = 48;

/** How one key moved in one committed callback. */
enum class Change : std::uint8_t {
    /** The key did not exist before. */
    added,
    /** The key existed with a different value. */
    changed,
    /** `clear_variable` removed the key. */
    cleared,
};

/** One retained change. */
struct Entry final {
    /** Activity id of the program that committed the change. */
    std::array<char, 64> activityId{};
    state::activity::mission::StateKey key{};
    /** The value after the change. Meaningless for `cleared`. */
    state::activity::mission::VariableValue value{};
    /** The value before the change. Meaningless for `added`. */
    state::activity::mission::VariableValue previous{};
    /** Starts at one and never repeats within a run. */
    std::uint64_t sequence{};
    /** `GetTickCount64` when the change was committed. */
    std::uint64_t tick{};
    Change change{Change::added};
};

/** Every retained change, oldest first. */
struct Snapshot final {
    std::array<Entry, kEntryCapacity> entries{};
    std::size_t count{};
    /** Sequence of the newest entry; zero while nothing was ever recorded. */
    std::uint64_t newestSequence{};
};

/**
 * Records every difference between the variables before and after one committed callback.
 * Both spans must be sorted by key, which is the order the VM keeps them in.
 * @param activityId Program that committed the change.
 */
void record_commit(std::string_view activityId,
                   std::span<const state::activity::mission::ScriptVariable> before,
                   std::span<const state::activity::mission::ScriptVariable> after) noexcept;

/** @return Sequence of the newest entry, so a reader can skip the copy when nothing changed. */
[[nodiscard]] std::uint64_t newest_sequence() noexcept;

/** Copies every retained change, oldest first. Safe from any thread. */
void snapshot(Snapshot& output) noexcept;

} // namespace sunrise::server::activity::mission::variable_log
