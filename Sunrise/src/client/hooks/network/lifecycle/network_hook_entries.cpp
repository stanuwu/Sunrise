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
        platform::transport_kind_session_entry_point(),
        http::execute_request_entry_point(),
        bubble_authority::decoder_entry_point(),
        bubble_authority::content_untracked_entry_point(),
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
        hooking::detour::Spec{resolved.transportKindSession, replacements[1]},
        hooking::detour::Spec{resolved.httpExecuteRequest, replacements[2]},
        hooking::detour::Spec{resolved.bubbleAuthorityDecoder, replacements[3]},
        hooking::detour::Spec{resolved.contentUntrackedGetter, replacements[4]},
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

/** @return The ingress and egress bodies; the replacements sit in .srguard, guarded as a span. */
GameProtectedEntries game_protected_entries() noexcept {
    return {hooking::detour::ProtectedCodeEntry{coordinator::ingress_entry_point()},
            hooking::detour::ProtectedCodeEntry{coordinator::egress_entry_point()}};
}

/** @return The ingress and egress bodies; the replacements sit in .srguard, guarded as a span. */
PlatformProtectedEntries platform_protected_entries() noexcept {
    return {hooking::detour::ProtectedCodeEntry{coordinator::ingress_entry_point()},
            hooking::detour::ProtectedCodeEntry{coordinator::egress_entry_point()}};
}

} // namespace sunrise::client::hooks::network::lifecycle
