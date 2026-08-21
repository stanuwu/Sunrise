#pragma once

#include <array>
#include <cstddef>

#include "key_bindings.h"

namespace sunrise::state::account::settings::bindings {

/** Array position is the fixed native account slot for the named semantic State action. */
inline constexpr std::array<Action, kActionCount> kActionsByNativeSlot{
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

/** Verifies that every semantic action owns exactly one native account slot. */
[[nodiscard]] consteval bool complete_native_slot_map() noexcept {
    std::array<bool, kActionCount> seen{};
    for (const Action action : kActionsByNativeSlot) {
        const std::size_t stateIndex = static_cast<std::size_t>(action);
        if (stateIndex >= seen.size() || seen[stateIndex]) {
            return false;
        }
        seen[stateIndex] = true;
    }
    return true;
}

static_assert(complete_native_slot_map());

} // namespace sunrise::state::account::settings::bindings
