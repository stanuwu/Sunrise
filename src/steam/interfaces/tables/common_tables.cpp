#include "internal.h"

namespace sunrise::steam::interfaces::tables {
namespace {

/** STEAMAPPS_INTERFACE_VERSION008 has 30 vtable slots. */
constexpr std::size_t kAppsMethodCount = 30;
/** SteamInput001 has 35 vtable slots. */
constexpr std::size_t kInputMethodCount = 35;
/** SteamUtils009 has 34 vtable slots. */
constexpr std::size_t kUtilsMethodCount = 34;
/** SteamFriends017 has 80 vtable slots. */
constexpr std::size_t kFriendsMethodCount = 80;
/** SteamUser020 has 31 vtable slots. */
constexpr std::size_t kUserMethodCount = 31;
/** STEAMUSERSTATS_INTERFACE_VERSION011 has 48 vtable slots. */
constexpr std::size_t kUserStatsMethodCount = 48;

/** Slots used from STEAMAPPS_INTERFACE_VERSION008. */
enum class AppsSlot : std::size_t {
    subscribed = 0,
    currentLanguage = 4,
    availableLanguages = 5,
    subscribedApp = 6,
    dlcInstalled = 7,
    dlcCount = 10,
    dlcData = 11,
    betaName = 15,
    installDir = 18,
    installed = 19,
    launchQueryParam = 21,
    buildId = 23,
};

/** Slots used from SteamInput001. */
enum class InputSlot : std::size_t {
    initialize = 0,
    shutdown = 1,
    actionSetHandle = 4,
    digitalActionHandle = 11,
    digitalActionData = 12,
    analogActionHandle = 14,
    analogActionData = 15,
    motionData = 20,
};

/** Slots used from SteamUtils009. */
enum class UtilsSlot : std::size_t {
    universe = 2,
    country = 4,
    appId = 9,
    ipv6Connectivity = 31,
    filterText = 32,
};

/** Slots used from SteamFriends017. */
enum class FriendsSlot : std::size_t {
    personaName = 0,
    overlayNeedsPresent = 49,
    richPresence = 64,
    inviteRichPresence = 65,
};

/** Slots used from SteamUser020. */
enum class UserSlot : std::size_t {
    handle = 0,
    loggedOn = 1,
    steamId = 2,
    requestEncryptedAppTicket = 20,
    getEncryptedAppTicket = 21,
};

/** Slots used from STEAMUSERSTATS_INTERFACE_VERSION011. */
enum class UserStatsSlot : std::size_t {
    requestCurrent = 0,
    achievement = 6,
    setAchievement = 7,
    store = 10,
};

std::array<FARPROC, kAppsMethodCount> g_appsMethods{};
std::array<FARPROC, kInputMethodCount> g_inputMethods{};
std::array<FARPROC, kUtilsMethodCount> g_utilsMethods{};
std::array<FARPROC, kFriendsMethodCount> g_friendsMethods{};
std::array<FARPROC, kUserMethodCount> g_userMethods{};
std::array<FARPROC, kUserStatsMethodCount> g_userStatsMethods{};
InterfaceObject g_apps{g_appsMethods.data()};
InterfaceObject g_input{g_inputMethods.data()};
InterfaceObject g_utils{g_utilsMethods.data()};
InterfaceObject g_friends{g_friendsMethods.data()};
InterfaceObject g_user{g_userMethods.data()};
InterfaceObject g_userStats{g_userStatsMethods.data()};

} // namespace

/** Sets up the common interface tables and fills the slots they use. */
void initialize_common() noexcept {
    fill_empty(g_appsMethods);
    fill_empty(g_inputMethods);
    fill_empty(g_utilsMethods);
    fill_empty(g_friendsMethods);
    fill_empty(g_userMethods);
    fill_empty(g_userStatsMethods);

    set_method(g_appsMethods[index(AppsSlot::subscribed)], &methods::return_true);
    set_method(g_appsMethods[index(AppsSlot::currentLanguage)], &methods::language);
    set_method(g_appsMethods[index(AppsSlot::availableLanguages)], &methods::language);
    set_method(g_appsMethods[index(AppsSlot::subscribedApp)], &methods::return_true);
    set_method(g_appsMethods[index(AppsSlot::dlcInstalled)], &methods::dlc_installed);
    set_method(g_appsMethods[index(AppsSlot::dlcCount)], &methods::get_dlc_count);
    set_method(g_appsMethods[index(AppsSlot::dlcData)], &methods::get_dlc_data);
    set_method(g_appsMethods[index(AppsSlot::betaName)], &methods::current_beta_name);
    set_method(g_appsMethods[index(AppsSlot::installDir)], &methods::app_install_dir);
    set_method(g_appsMethods[index(AppsSlot::installed)], &methods::app_is_installed);
    set_method(g_appsMethods[index(AppsSlot::launchQueryParam)], &methods::launch_query_param);
    set_method(g_appsMethods[index(AppsSlot::buildId)], &methods::app_build_id);

    set_method(g_inputMethods[index(InputSlot::initialize)], &methods::return_true);
    set_method(g_inputMethods[index(InputSlot::shutdown)], &methods::return_true);
    set_method(g_inputMethods[index(InputSlot::actionSetHandle)], &methods::input_handle);
    set_method(g_inputMethods[index(InputSlot::digitalActionHandle)], &methods::input_handle);
    set_method(g_inputMethods[index(InputSlot::digitalActionData)], &methods::input_digital_data);
    set_method(g_inputMethods[index(InputSlot::analogActionHandle)], &methods::input_handle);
    set_method(g_inputMethods[index(InputSlot::analogActionData)], &methods::input_analog_data);
    set_method(g_inputMethods[index(InputSlot::motionData)], &methods::input_motion_data);

    set_method(g_utilsMethods[index(UtilsSlot::universe)], &methods::connected_universe);
    set_method(g_utilsMethods[index(UtilsSlot::country)], &methods::country);
    set_method(g_utilsMethods[index(UtilsSlot::appId)], &methods::get_app_id);
    set_method(g_utilsMethods[index(UtilsSlot::ipv6Connectivity)], &methods::return_true);
    set_method(g_utilsMethods[index(UtilsSlot::filterText)], &methods::filter_text);

    set_method(g_friendsMethods[index(FriendsSlot::personaName)], &methods::persona_name);
    set_method(g_friendsMethods[index(FriendsSlot::overlayNeedsPresent)], &methods::return_true);
    set_method(g_friendsMethods[index(FriendsSlot::richPresence)], &methods::return_true);
    set_method(g_friendsMethods[index(FriendsSlot::inviteRichPresence)], &methods::return_true);

    set_method(g_userMethods[index(UserSlot::handle)], &methods::get_user_handle);
    set_method(g_userMethods[index(UserSlot::loggedOn)], &methods::return_true);
    set_method(g_userMethods[index(UserSlot::steamId)], &methods::get_steam_id);
    set_method(g_userMethods[index(UserSlot::requestEncryptedAppTicket)],
               &methods::request_encrypted_app_ticket);
    set_method(g_userMethods[index(UserSlot::getEncryptedAppTicket)],
               &methods::get_encrypted_app_ticket);

    set_method(g_userStatsMethods[index(UserStatsSlot::requestCurrent)],
               &methods::request_current_stats);
    set_method(g_userStatsMethods[index(UserStatsSlot::achievement)], &methods::get_achievement);
    set_method(g_userStatsMethods[index(UserStatsSlot::setAchievement)], &methods::return_true);
    set_method(g_userStatsMethods[index(UserStatsSlot::store)], &methods::return_true);
}

/** @return The apps interface. */
void* apps() noexcept {
    return &g_apps;
}
/** @return The input interface. */
void* input() noexcept {
    return &g_input;
}
/** @return The utils interface. */
void* utils() noexcept {
    return &g_utils;
}
/** @return The friends interface. */
void* friends() noexcept {
    return &g_friends;
}
/** @return The user interface. */
void* user() noexcept {
    return &g_user;
}
/** @return The user stats interface. */
void* user_stats() noexcept {
    return &g_userStats;
}

} // namespace sunrise::steam::interfaces::tables
