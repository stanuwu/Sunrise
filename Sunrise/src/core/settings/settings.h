#pragma once

#include <string_view>

#include "../../state/account/account_state.h"
#include "../../state/activity/defaults/definition.h"
#include "../../state/investment/investment.h"
#include "../../state/unlocks/definition.h"
#include "../logging/log.h"
#include "client/definition.h"
#include "role.h"
#include "server/definition.h"
#include "steam/definition.h"

namespace sunrise::core::settings {

/** Activity SDK generator policy. Generation has no switch, because the host needs the SDK. */
struct ActivitySdkGenerationSettings final {
    /** Writes the sdk/lua declaration tree. */
    bool luaDeclarations{true};
};

/**
 * Layout version of the settings file this build writes and expects.
 * Raise it when a key is renamed, removed, changes meaning, or must take a new default. Adding a
 * key needs no raise, because a missing key already takes its default.
 */
inline constexpr std::uint32_t kSettingsVersion = 19;

/** IPv4 loopback address in network byte order, shared by settings and endpoint policy. */
inline constexpr std::array<unsigned char, 4> kLoopbackOctets{127, 0, 0, 1};

/** Parsed read-only process settings. */
struct Settings {
    /**
     * Layout version supplied by the file, defaulting to kSettingsVersion when absent.
     * Loading replaces older versioned files; compact unversioned files are preserved.
     */
    std::uint32_t version{};
    /** Explicit permission to host or join multiplayer and allow its network endpoints. */
    bool multiplayerEnabled{};
    bool compactClient{};
    bool compactHost{};
    Role configuredRole{Role::embedded};
    bool hasConfiguredRole{};
    /**
     * Completes released exotic weapon catalysts while resolving client item state.
     */
    bool completeExoticCatalysts{true};
    /** Core-owned sink and channel policy. */
    log::Settings logging;
    /** Core-owned boot gate for activity SDK generation. */
    ActivitySdkGenerationSettings activitySdkGeneration;
    /** Options used only by the Client layer. */
    client::Settings client;
    /** Options used only by the Server layer. */
    server::Settings server;
    /** Options used only by the Steam compatibility layer. */
    steam::Settings steam;
    /** Small local destination fallback published when State starts. */
    state::activity::defaults::ActivityDefaults initialActivityDefaults;
};

/** @return The complete default settings. */
[[nodiscard]] Settings defaults() noexcept;

/** Reason a settings document was refused. */
enum class ParseFailure { none, invalidDocument, multiplayerOptInRequired };

/**
 * Parses supported JSON settings on top of the defaults.
 * @param json Complete settings text.
 * @param output Receives the settings only after the whole document is valid.
 * @param failure Optional specific refusal reason.
 * @return True when the document matches the supported settings.
 */
[[nodiscard]] bool
parse(std::string_view json, Settings& output, ParseFailure* failure = nullptr) noexcept;

/**
 * Loads the settings file next to the module when it is there.
 * @param module Loaded DLL, used to find the settings path.
 * @return True when defaults or a valid settings file are active.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Resets active settings to the fixed defaults. */
void shutdown() noexcept;

/** @return Active read-only Core settings. */
[[nodiscard]] const Settings& get() noexcept;

/** Explicit opt-in enables native multiplayer adapters for a host or an upstream client. */
[[nodiscard]] inline bool multiplayer() noexcept {
    return get().multiplayerEnabled && (role() == Role::host || get().server.upstream.enabled);
}

} // namespace sunrise::core::settings
