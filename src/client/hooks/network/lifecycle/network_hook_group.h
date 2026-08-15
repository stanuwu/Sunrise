#pragma once

#include <array>
#include <span>

#include "../platform.h"

namespace sunrise::client::hooks::network::lifecycle {

/**
 * Game-owned hooks needed before the first SignOn request. The BAP transport is not one of them:
 * the Client reaches the loopback listener over a real socket, which is what lets the Server send
 * a frame the Client did not ask for.
 */
inline constexpr std::array kGameSlots{
    HookSlot::transportKind,
    HookSlot::httpExecuteRequest,
    HookSlot::bubbleAuthorityDecoder,
    HookSlot::contentUntrackedGetter,
    HookSlot::signOnReadinessFailure,
    HookSlot::signOnReadinessReady,
};

/** Steam networking hooks, found after SteamAPI_Init. */
inline constexpr std::array kPlatformSlots{
    HookSlot::authenticationStatus,
    HookSlot::setCertificate,
};

/** @param slots Stable hook slots. @return True when every slot is attached. */
[[nodiscard]] bool all_installed(std::span<const HookSlot> slots) noexcept;

/** @param slots Stable hook slots. @return True when at least one slot is attached. */
[[nodiscard]] bool any_installed(std::span<const HookSlot> slots) noexcept;

/** @param slots Stable hook slots. @return True when every slot accepts calls. */
[[nodiscard]] bool all_accepting(std::span<const HookSlot> slots) noexcept;

/** @param slots Stable hook slots. @param accepting New admission state. */
void set_accepting(std::span<const HookSlot> slots, bool accepting) noexcept;

/**
 * Installs one complete group while the caller owns the lifecycle lock.
 * @param specs Target and replacement pairs in group order.
 * @param slots Output slots paired with the specs.
 * @return True when every group handle and admission flag is set.
 */
[[nodiscard]] bool install_group(std::span<const hooking::detour::Spec> specs,
                                 std::span<const HookSlot> slots) noexcept;

/**
 * Removes one complete group while the caller owns the lifecycle lock.
 * @param slots Handles owned by the group.
 * @param protectedEntries Replacement and coordinator bodies that must be idle.
 * @return True when the group is gone, or every handle was removed.
 */
[[nodiscard]] bool
uninstall_group(std::span<const HookSlot> slots,
                std::span<const hooking::detour::ProtectedCodeEntry> protectedEntries) noexcept;

} // namespace sunrise::client::hooks::network::lifecycle
