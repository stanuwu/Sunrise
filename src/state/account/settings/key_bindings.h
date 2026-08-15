#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sunrise::state::account::settings::bindings {

/** Authored actions whose primary and secondary inputs are replicated with account settings. */
enum class Action : std::uint8_t {
    fire,
    toggleZoom,
    holdZoom,
    melee,
    grenade,
    super,
    reload,
    lightAttack,
    heavyAttack,
    block,
    switchWeapons,
    nextWeapon,
    previousWeapon,
    primaryWeapon,
    specialWeapon,
    heavyWeapon,
    moveForward,
    moveBackward,
    moveLeft,
    moveRight,
    jump,
    toggleCrouch,
    holdCrouch,
    toggleSprint,
    holdSprint,
    vehicleBoost,
    vehicleBrake,
    vehicleZoom,
    vehicleFirePrimary,
    vehicleFireSecondary,
    vehicleExit,
    interact,
    highlightPlayer,
    emoteOne,
    emoteTwo,
    emoteThree,
    emoteFour,
    airMove,
    classAbility,
    deathCameraZoomIn,
    deathCameraZoomOut,
    pushToTalk,
    uiGamepadButtonBack,
    uiOpenDirector,
    uiOpenDirectorStoreTab,
    uiOpenDirectorPursuitsTab,
    uiOpenDirectorMapTab,
    uiOpenDirectorDestinationsTab,
    uiOpenDirectorRosterTab,
    uiOpenDirectorSeasonsTab,
    uiOpenStartMenuAlternative,
    uiOpenStartMenuRecordsTab,
    uiOpenStartMenuCollectionsTab,
    uiOpenStartMenuClanTab,
    uiOpenStartMenuInventoryTab,
    uiOpenStartMenuSettingsTab,
    uiOpenExitDialogConfirm,
    uiAbortActivity,
    uiTextChatToggleState,
    screenshot,
    count,
};

/** The replicated input-action table has exactly one row per supported action. */
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::count);

/** Authored input codes. A missing half means that primary or secondary input is unbound. */
struct Binding {
    std::optional<std::uint16_t> primary;
    std::optional<std::uint16_t> secondary;
};

/** Fixed authored input table independent of the native packed account representation. */
struct KeyBindings {
    std::array<Binding, kActionCount> values;
    /** True only when configuration supplied every supported action. */
    bool configured{};
};

} // namespace sunrise::state::account::settings::bindings
