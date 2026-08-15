// The content state machine asks the config singleton for a URL and a token before it fetches.
// Nothing fills that singleton here, so both answers must come from Sunrise.

#include "config_getter_lifecycle.h"

#include <Windows.h>

#include <array>
#include <cstddef>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../targets/game/config_getter.h"
#include "config_getter_answers.h"

namespace sunrise::client::hooks::config_getter {

namespace {

/** Slot of the URL getter in the transaction; the spec and handle spans share this order. */
constexpr std::size_t kUrlSlot = 0;
/** Slot of the token getter in the same order. */
constexpr std::size_t kTokenSlot = 1;
/** The transaction covers exactly these two getters. */
constexpr std::size_t kGetterCount = 2;

SRWLOCK g_lock{SRWLOCK_INIT};

/** Both getters attach in one transaction, so the Client never sees only one answered. */
std::array<hooking::detour::Handle, kGetterCount> g_hooks{};

/** @return True while both getters answer. */
[[nodiscard]] bool attached() noexcept {
    return g_hooks[kUrlSlot].attached && g_hooks[kTokenSlot].attached;
}

} // namespace

/** Answers the Client's config URL and token questions by detouring both getters. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (attached()) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (!targets::game::config_getter::is_resolved()) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=config_getter stage=install result=fail reason=target");
        return true;
    }

    const targets::game::config_getter::Targets& resolved = targets::game::config_getter::get();
    const std::array<hooking::detour::Spec, kGetterCount> specs{
        hooking::detour::Spec{resolved.url, url_entry_point()},
        hooking::detour::Spec{resolved.token, token_entry_point()}};
    const bool installed = hooking::detour::install(specs, g_hooks);
    ReleaseSRWLockExclusive(&g_lock);

    core::log::write(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     installed ? "ev=config_getter stage=install result=ok"
                               : "ev=config_getter stage=install result=fail reason=attach");
    return installed;
}

/** @return True when neither getter is detoured. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    // A batch removal rejects detached handles, so never-installed is already the wanted state.
    const bool removed = !attached() || hooking::detour::uninstall(g_hooks);
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

/** @return True while both getters answer. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = attached();
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

} // namespace sunrise::client::hooks::config_getter
