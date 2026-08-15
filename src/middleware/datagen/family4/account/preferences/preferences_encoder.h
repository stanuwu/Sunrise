#pragma once

#include "../../../../../state/account/settings/settings_state.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::account::preferences {

/**
 * Maps semantic State preferences into the two native account records.
 * @param settings Complete validated account preferences and keybindings.
 * @param record Receives the byte-exact menu preference fields.
 * @param bindingsRecord Receives client-seed mirrors and packed keybindings.
 * @return True when the complete settings satisfy the State contract and are encoded.
 */
[[nodiscard]] bool encode(const state::account::settings::AccountSettings& settings,
                          Record& record,
                          BindingsRecord& bindingsRecord) noexcept;

} // namespace sunrise::middleware::datagen::family4::account::preferences
