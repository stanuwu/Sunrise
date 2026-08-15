#pragma once

#include <span>

namespace sunrise::client::hooking::detour {

/** Describes one target-to-replacement attachment. */
struct Spec {
    void* target{};
    void* replacement{};
};

/** Owns the trampoline state returned by an attached detour. */
struct Handle {
    void* original{};
    void* replacement{};
    bool attached{};
};

/** One function that must be idle before a detour is removed. */
struct ProtectedCodeEntry {
    void* address{};
};

/** Lock-free, non-waiting idle test, run after Detours suspends the transaction threads. */
using IdleCheck = bool (*)() noexcept;

/** Final state of a protected detour removal request. */
enum class UninstallResult {
    removed,
    protectedCodeActive,
    failed,
};

/** Attaches one detour and publishes its trampoline in output. */
[[nodiscard]] bool install(const Spec& spec, Handle& output) noexcept;

/** Detaches one detour and clears its handle on success. */
[[nodiscard]] bool uninstall(Handle& handle) noexcept;

/** Detaches one detour and reports a suspended call inside its replacement body. */
[[nodiscard]] bool uninstall(Handle& handle, bool& replacementActive) noexcept;

/** Detaches one detour only when every protected function is idle. */
[[nodiscard]] UninstallResult
uninstall(Handle& handle, std::span<const ProtectedCodeEntry> protectedEntries) noexcept;

/** Detaches one detour only when protected code and caller-owned work are idle. */
[[nodiscard]] UninstallResult uninstall(Handle& handle,
                                        std::span<const ProtectedCodeEntry> protectedEntries,
                                        IdleCheck idleCheck) noexcept;

/** Attaches all specs in one transaction or leaves every output detached. */
[[nodiscard]] bool install(std::span<const Spec> specs, std::span<Handle> outputs) noexcept;

/** Detaches all handles in one transaction. */
[[nodiscard]] bool uninstall(std::span<Handle> handles) noexcept;

/** Detaches all handles and reports a suspended call inside any replacement body. */
[[nodiscard]] bool uninstall(std::span<Handle> handles, bool& replacementActive) noexcept;

/** Detaches all handles only when every protected function is idle. */
[[nodiscard]] UninstallResult
uninstall(std::span<Handle> handles, std::span<const ProtectedCodeEntry> protectedEntries) noexcept;

/** Detaches all handles only when protected code and caller-owned work are idle. */
[[nodiscard]] UninstallResult uninstall(std::span<Handle> handles,
                                        std::span<const ProtectedCodeEntry> protectedEntries,
                                        IdleCheck idleCheck) noexcept;

} // namespace sunrise::client::hooking::detour
