#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace sunrise::client::hooks::machine_id::cache {
/** Borrowed native transport fields; the caller keeps them valid until the override is removed. */
struct Fields {
    std::uint8_t* initialized{};
    void* record{};
    void* id{};
};
/** Native composer that fills in `record` before `id` is read for the first time. */
using Compose = void (*)(void*, void*);
/** Protected write into the client image. False when the page could not be made writable. */
using Write = bool (*)(void*, const void*, std::size_t) noexcept;
/** One active override, plus the native id it displaced so `uninstall` can put it back. */
struct Override {
    Fields fields{};
    std::uint64_t original{};
    std::uint64_t requested{};
    bool active{};
};
/** @return The machine id currently stored at `fields.id`. */
inline std::uint64_t read_id(const Fields& fields) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, fields.id, sizeof value);
    return value;
}
/**
 * Composes the record if the native producer has not yet, then writes `requested` into it.
 * @param requested Zero means no override is configured; returns true without touching anything.
 * @param output Receives the active override. An already-active override succeeds only when it
 * targets the same id slot and the same requested value.
 * @return False for an incomplete `Fields`, a missing composer or write callback, an all-ones
 * `requested`, or a refused write. `output` is left unchanged on false.
 */
inline bool install(const Fields& fields,
                    Compose compose,
                    Write write,
                    std::uint64_t requested,
                    Override& output) noexcept {
    if (!requested) {
        return true;
    }
    if (!fields.initialized || !fields.record || !fields.id || !compose || !write
        || requested == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (output.active) {
        return output.fields.id == fields.id && output.requested == requested;
    }
    if (!*fields.initialized) {
        compose(fields.record, fields.id);
        const std::uint8_t initialized = 1;
        if (!write(fields.initialized, &initialized, sizeof initialized)) {
            return false;
        }
    }
    const auto original = read_id(fields);
    if (!write(fields.id, &requested, sizeof requested)) {
        return false;
    }
    output = {fields, original, requested, true};
    return true;
}
/** @return False only when the cache differs from `requested` and rewriting it failed. */
inline bool poll(const Override& state, Write write) noexcept {
    return !state.active || read_id(state.fields) == state.requested
           || (write && write(state.fields.id, &state.requested, sizeof state.requested));
}
/**
 * Restores the native id when the cache still holds `requested`, then clears the override.
 * @return False only when the restore write was refused; the override stays active for a later
 * retry. A cache value a later native producer already replaced is left alone and the override
 * is dropped without restoring it.
 */
inline bool uninstall(Override& state, Write write) noexcept {
    if (!state.active) {
        return true;
    }
    // Skips the restore when the cache holds a value this override did not write; a later
    // native producer already claimed it.
    if (read_id(state.fields) == state.requested
        && (!write || !write(state.fields.id, &state.original, sizeof state.original))) {
        return false;
    }
    state = {};
    return true;
}
} // namespace sunrise::client::hooks::machine_id::cache
