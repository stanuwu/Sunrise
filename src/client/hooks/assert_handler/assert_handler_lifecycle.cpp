#include "assert_handler_lifecycle.h"

#include "../../../core/logging/log.h"
#include "../../targets/game/assert_handler.h"
#include "assert_handler_observer.h"

namespace sunrise::client::hooks::assert_handler {

SRWLOCK g_lock{SRWLOCK_INIT};
bool g_installed{};

namespace {

/**
 * Exchanges the handler slot under its own page protection.
 * @param slot Resolved handler slot.
 * @param expected Value the slot must currently hold.
 * @param value Replacement pointer.
 * @return True when the slot holds the replacement after the call.
 */
[[nodiscard]] bool exchange(std::byte** slot, const void* expected, void* value) noexcept {
    if (slot == nullptr || *slot != expected) {
        return false;
    }
    // The slot is a writable data global, so the write needs no protection change. The game's own
    // setter stores it with a plain move. TODO: call that setter instead of writing the slot.
    *slot = static_cast<std::byte*>(value);
    return true;
}

} // namespace

/** Replaces the game's fatal assert handler so an assert reports instead of halting. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_installed) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (!targets::game::assert_handler::is_resolved()) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=assert stage=install result=fail reason=target");
        return true;
    }
    const targets::game::assert_handler::Targets& resolved = targets::game::assert_handler::get();
    const bool installed = exchange(resolved.slot, resolved.original, handler_entry_point());
    g_installed = installed;
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     installed ? "ev=assert stage=install result=ok"
                               : "ev=assert stage=install result=fail reason=slot");
    return installed;
}

/** Restores the game's own handler so a later assert cannot call into unmapped code. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_installed) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    const targets::game::assert_handler::Targets& resolved = targets::game::assert_handler::get();
    const bool restored = exchange(resolved.slot, handler_entry_point(), resolved.original);
    g_installed = !restored;
    ReleaseSRWLockExclusive(&g_lock);
    return restored;
}

/** @return True while Sunrise's handler owns the slot. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = g_installed;
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

} // namespace sunrise::client::hooks::assert_handler
