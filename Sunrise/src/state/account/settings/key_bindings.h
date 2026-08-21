#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sunrise::state::account::settings::bindings {

/** Low byte of a native input code. The high byte carries at most one modifier flag. */
inline constexpr std::uint16_t kInputCodeMask = 0x00FF;
inline constexpr std::uint16_t kModifierMask = 0xFF00;
/** Native table value used on the wire to mean that a binding half is empty. */
inline constexpr std::uint16_t kUnboundInputCode = 0x0074;
/** Highest real input index. The following value is the unbound sentinel, not a key. */
inline constexpr std::uint16_t kMaximumBindableInputCode = kUnboundInputCode - 1;
/** Native modifier flags occupy one mutually exclusive bit above the low-byte input index. */
inline constexpr std::uint16_t kAltModifierFlag = 0x0100;
inline constexpr std::uint16_t kControlModifierFlag = 0x0200;
inline constexpr std::uint16_t kShiftModifierFlag = 0x0400;

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

    bool operator==(const Binding&) const = default;
};

/** Fixed authored input table independent of the native packed account representation. */
struct KeyBindings {
    std::array<Binding, kActionCount> values;
    /** True only when configuration supplied every supported action. */
    bool configured{};

    bool operator==(const KeyBindings&) const = default;
};

} // namespace sunrise::state::account::settings::bindings
