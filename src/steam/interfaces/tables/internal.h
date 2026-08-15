#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

#include "../internal.h"

namespace sunrise::steam::interfaces::tables {

/** A Steam interface object. It holds only its vtable pointer. */
struct InterfaceObject {
    FARPROC* methods{};
};

/** @param slot Typed ABI slot. @return Its vtable array index. */
template <typename Slot> [[nodiscard]] constexpr std::size_t index(Slot slot) noexcept {
    return static_cast<std::size_t>(slot);
}

/** Fills every vtable slot with the safe do-nothing method. */
template <std::size_t Count> void fill_empty(std::array<FARPROC, Count>& table) noexcept {
    table.fill(reinterpret_cast<FARPROC>(&methods::empty));
}

/** Stores one typed method in an untyped Steam vtable slot. */
template <typename Function> void set_method(FARPROC& slot, Function function) noexcept {
    slot = reinterpret_cast<FARPROC>(function);
}

/** Sets up the Apps, Input, Utils, Friends, User and UserStats tables. */
void initialize_common() noexcept;

/** Sets up the Matchmaking, Client, serialized-networking and HTTP tables. */
void initialize_networking() noexcept;

} // namespace sunrise::steam::interfaces::tables
