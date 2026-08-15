#pragma once

#include <array>
#include <cstddef>

#include "../../../../../state/account/settings/key_bindings.h"

namespace sunrise::middleware::datagen::family4::account::preferences {

using state::account::settings::bindings::Action;

/** Array position is the fixed native account slot for the named semantic State action. */
inline constexpr std::array<Action, state::account::settings::bindings::kActionCount>
    kActionsBySlot{
        Action::fire,
        Action::toggleZoom,
        Action::holdZoom,
        Action::melee,
        Action::grenade,
        Action::super,
        Action::reload,
        Action::lightAttack,
        Action::heavyAttack,
        Action::block,
        Action::switchWeapons,
        Action::nextWeapon,
        Action::previousWeapon,
        Action::primaryWeapon,
        Action::specialWeapon,
        Action::heavyWeapon,
        Action::moveForward,
        Action::moveBackward,
        Action::moveLeft,
        Action::moveRight,
        Action::jump,
        Action::toggleCrouch,
        Action::holdCrouch,
        Action::toggleSprint,
        Action::holdSprint,
        Action::vehicleBoost,
        Action::vehicleBrake,
        Action::vehicleZoom,
        Action::vehicleFirePrimary,
        Action::vehicleFireSecondary,
        Action::vehicleExit,
        Action::interact,
        Action::highlightPlayer,
        Action::emoteOne,
        Action::emoteTwo,
        Action::emoteThree,
        Action::emoteFour,
        Action::airMove,
        Action::classAbility,
        Action::deathCameraZoomIn,
        Action::deathCameraZoomOut,
        Action::pushToTalk,
        Action::uiGamepadButtonBack,
        Action::uiOpenDirector,
        Action::uiOpenDirectorStoreTab,
        Action::uiOpenDirectorPursuitsTab,
        Action::uiOpenDirectorMapTab,
        Action::uiOpenDirectorDestinationsTab,
        Action::uiOpenDirectorRosterTab,
        Action::uiOpenDirectorSeasonsTab,
        Action::uiOpenStartMenuAlternative,
        Action::uiOpenStartMenuRecordsTab,
        Action::uiOpenStartMenuCollectionsTab,
        Action::uiOpenStartMenuClanTab,
        Action::uiOpenStartMenuInventoryTab,
        Action::uiOpenStartMenuSettingsTab,
        Action::uiOpenExitDialogConfirm,
        Action::uiAbortActivity,
        Action::uiTextChatToggleState,
        Action::screenshot,
    };

/**
 * Verifies that the native slot table contains every semantic action once.
 * @return True when the table is a complete one-to-one mapping.
 */
[[nodiscard]] consteval bool complete() noexcept {
    std::array<bool, state::account::settings::bindings::kActionCount> seen{};
    for (const Action action : kActionsBySlot) {
        const std::size_t stateIndex = static_cast<std::size_t>(action);
        // Every State action must own exactly one native account slot.
        if (stateIndex >= seen.size() || seen[stateIndex]) {
            return false;
        }
        seen[stateIndex] = true;
    }
    return true;
}

static_assert(complete());

} // namespace sunrise::middleware::datagen::family4::account::preferences
