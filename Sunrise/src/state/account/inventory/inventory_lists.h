#pragma once

#include <array>
#include <cstddef>

namespace sunrise::state::account::inventory {

/**
 * Removes the rows a dense list does not keep, in order. Every account list needs an empty tail to
 * stay valid, so the freed rows are cleared.
 * @param values List storage; rows [0, count) are used.
 * @param count Used rows, lowered to the rows kept.
 * @param keep Returns true for a row that stays.
 * @param removed Called with the old position and the row, for each row that goes.
 */
template <typename T, std::size_t N, typename Keep, typename Removed>
void compact(std::array<T, N>& values, std::size_t& count, Keep keep, Removed removed) {
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (keep(values[index])) {
            values[kept++] = values[index];
        } else {
            removed(index, values[index]);
        }
    }
    for (std::size_t index = kept; index < count; ++index) {
        values[index] = T{};
    }
    count = kept;
}

/** Removes one row of a dense list and clears the freed tail row. */
template <typename T, std::size_t N>
void erase(std::array<T, N>& values, std::size_t& count, std::size_t index) {
    compact(
        values,
        count,
        [&values, index](const T& row) { return &row != &values[index]; },
        [](std::size_t, const T&) {});
}

} // namespace sunrise::state::account::inventory
