#include "network_hook_entries.h"

#include "../../../targets/game.h"
#include "../../../targets/steam_targets.h"
#include "../bubble_authority/bubble_authority_replacements.h"
#include "../coordinator/network_call_coordinator.h"

namespace sunrise::client::hooks::network::lifecycle {
namespace {

/** @return The game replacement bodies, in hook-slot order. */
[[nodiscard]] std::array<void*, kGameSlots.size()> game_replacements() noexcept {
    return {
        platform::transport_kind_entry_point(),
        http::execute_request_entry_point(),
        bubble_authority::decoder_entry_point(),
        bubble_authority::content_untracked_entry_point(),
        signon::readiness_entry_point(),
        signon::ready_entry_point(),
    };
}

/** @return The platform replacement bodies, in hook-slot order. */
[[nodiscard]] std::array<void*, kPlatformSlots.size()> platform_replacements() noexcept {
    return {
        platform::authentication_status_entry_point(),
        platform::set_certificate_entry_point(),
    };
}

} // namespace

/** @return Game target and body pairs, in slot order. */
GameSpecs game_specs() noexcept {
    const auto replacements = game_replacements();
    const targets::game::network::Targets& resolved = targets::game::network::get();
    return {
        hooking::detour::Spec{resolved.transportKind, replacements[0]},
        hooking::detour::Spec{resolved.httpExecuteRequest, replacements[1]},
        hooking::detour::Spec{resolved.bubbleAuthorityDecoder, replacements[2]},
        hooking::detour::Spec{resolved.contentUntrackedGetter, replacements[3]},
        hooking::detour::Spec{resolved.signOnReadinessFailure, replacements[4]},
        hooking::detour::Spec{resolved.signOnReadinessReady, replacements[5]},
    };
}

/** @return Platform target and body pairs, in slot order. */
PlatformSpecs platform_specs() noexcept {
    const auto replacements = platform_replacements();
    const targets::steam::Targets& resolved = targets::steam::get();
    return {
        hooking::detour::Spec{resolved.authenticationStatus, replacements[0]},
        hooking::detour::Spec{resolved.setCertificate, replacements[1]},
    };
}

/** @return The game replacement, ingress and egress bodies kept safe at detach. */
GameProtectedEntries game_protected_entries() noexcept {
    const auto replacements = game_replacements();
    GameProtectedEntries entries{};
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        entries[index].address = replacements[index];
    }
    entries[replacements.size()].address = coordinator::ingress_entry_point();
    entries[replacements.size() + 1].address = coordinator::egress_entry_point();
    return entries;
}

/** @return The platform replacement, ingress and egress bodies kept safe at detach. */
PlatformProtectedEntries platform_protected_entries() noexcept {
    const auto replacements = platform_replacements();
    PlatformProtectedEntries entries{};
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        entries[index].address = replacements[index];
    }
    entries[replacements.size()].address = coordinator::ingress_entry_point();
    entries[replacements.size() + 1].address = coordinator::egress_entry_point();
    return entries;
}

} // namespace sunrise::client::hooks::network::lifecycle
