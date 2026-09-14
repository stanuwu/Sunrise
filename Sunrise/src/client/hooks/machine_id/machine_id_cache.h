#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace sunrise::client::hooks::machine_id::cache {
/** Native composition initializes the surrounding transport record before its ID is replaced. */
struct Fields {
    std::uint8_t* initialized{};
    void* record{};
    void* id{};
};
using Compose = void (*)(void*, void*);
using Write = bool (*)(void*, const void*, std::size_t) noexcept;
struct Override {
    Fields fields{};
    std::uint64_t original{};
    std::uint64_t requested{};
    bool active{};
};
inline std::uint64_t read_id(const Fields& fields) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, fields.id, sizeof value);
    return value;
}
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
inline bool poll(const Override& state, Write write) noexcept {
    return !state.active || read_id(state.fields) == state.requested
           || (write && write(state.fields.id, &state.requested, sizeof state.requested));
}
inline bool uninstall(Override& state, Write write) noexcept {
    if (!state.active) {
        return true;
    }
    // Preserve a later native producer's value if this override no longer owns the cache.
    if (read_id(state.fields) == state.requested
        && (!write || !write(state.fields.id, &state.original, sizeof state.original))) {
        return false;
    }
    state = {};
    return true;
}
} // namespace sunrise::client::hooks::machine_id::cache
