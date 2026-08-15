#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <span>

#include "../../detour.h"

namespace sunrise::client::hooking::detour::transaction {

/** Fixed limit on the handles held while process threads are suspended. */
constexpr std::size_t kThreadLimit = 1024;

/** Owns the process thread handles enlisted in one Detours transaction. */
struct Threads {
    std::array<HANDLE, kThreadLimit> handles{};
    std::array<DWORD, kThreadLimit> ids{};
    std::size_t count{};
};

/** Result of checking suspended instruction pointers. */
enum class InspectionResult {
    clear,
    protectedCodeActive,
    failed,
};

/** Starts a transaction and suspends every observed process thread. */
[[nodiscard]] bool begin(Threads& threads) noexcept;

/** Aborts the current transaction and releases its thread handles. */
[[nodiscard]] bool abort(Threads& threads) noexcept;

/** Commits the current transaction and releases its thread handles. */
[[nodiscard]] bool commit(Threads& threads) noexcept;

/** Checks all suspended instruction pointers against protected function ranges. */
[[nodiscard]] InspectionResult inspect(const Threads& threads,
                                       std::span<const ProtectedCodeEntry> entries) noexcept;

} // namespace sunrise::client::hooking::detour::transaction
