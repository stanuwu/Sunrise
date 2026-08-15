#pragma once

#include <string_view>

#include "../../state/account/account_state.h"
#include "../../state/activity/defaults/definition.h"
#include "../../state/investment/investment.h"
#include "../../state/unlocks/definition.h"
#include "../logging/log.h"
#include "client/definition.h"
#include "server/definition.h"
#include "steam/definition.h"

namespace sunrise::core::settings {

/**
 * Layout version of the settings file this build writes and expects.
 * Raise it when a key is renamed, removed, or changes meaning. Adding a key needs no raise,
 * because a missing key already takes its default. An older file is upgraded in place at load.
 */
inline constexpr std::uint32_t kSettingsVersion = 3;

/** Parsed read-only process settings. */
struct Settings {
    /**
     * Layout version the file was written against. Zero means the key was missing, which is
     * every file written before versioning. Checked against kSettingsVersion at load.
     */
    std::uint32_t version{};
    /** Core-owned sink and channel policy. */
    log::Settings logging;
    /** Options used only by the Client layer. */
    client::Settings client;
    /** Options used only by the Server layer. */
    server::Settings server;
    /** Options used only by the Steam compatibility layer. */
    steam::Settings steam;
    /** Complete authored account State, or an empty account. */
    state::AccountState initialAccount;
    /** Small local destination fallback published when State starts. */
    state::activity::defaults::ActivityDefaults initialActivityDefaults;
    /** Authored acquired-flag and objective policy published into the account object. */
    state::unlocks::Table initialUnlocks;
    /** Authored family-5 unlock overrides. Only the two override lists are authored here. */
    state::Family5State initialFamily5;
};

/** @return The complete default settings. */
[[nodiscard]] Settings defaults() noexcept;

/**
 * Parses supported JSON settings on top of the defaults.
 * @param json Complete settings text.
 * @param output Receives the settings only after the whole document is valid.
 * @return True when the document matches the supported settings.
 */
[[nodiscard]] bool parse(std::string_view json, Settings& output) noexcept;

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

} // namespace sunrise::core::settings
