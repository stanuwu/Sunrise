#include "steam_interface_factory.h"

#include <array>
#include <cstring>

#include "internal.h"

namespace sunrise::steam::interfaces {
namespace {

struct UserInterfaceEntry {
    const char* version{};
    void* (*get)() noexcept {};
};

/** Supported per-user interface versions and their table accessors. */
constexpr std::array kUserInterfaces{
    UserInterfaceEntry{versions::kApps, &tables::apps},
    UserInterfaceEntry{versions::kInput, &tables::input},
    UserInterfaceEntry{versions::kUtils, &tables::utils},
    UserInterfaceEntry{versions::kFriends, &tables::friends},
    UserInterfaceEntry{versions::kUser, &tables::user},
    UserInterfaceEntry{versions::kUserStats, &tables::user_stats},
    UserInterfaceEntry{versions::kMatchmaking, &tables::matchmaking},
};

} // namespace

/** Creates one supported Steam client interface. */
void* create(const char* version) noexcept {
    if (version == nullptr || !tables::initialize()) {
        return nullptr;
    }
    return std::strcmp(version, versions::kLegacyClient) == 0 ? tables::client() : nullptr;
}

/** Returns one supported interface for the active Steam user. */
void* find_user(const char* version) noexcept {
    if (version == nullptr || !tables::initialize()) {
        return nullptr;
    }
    for (const auto& entry : kUserInterfaces) {
        if (std::strcmp(version, entry.version) == 0) {
            return entry.get();
        }
    }
    return nullptr;
}

/** Returns the process-global interface. It needs no local user handle. */
void* find_global(const char* version) noexcept {
    if (version == nullptr || !tables::initialize()) {
        return nullptr;
    }
    return std::strcmp(version, versions::kUtils) == 0 ? tables::utils() : nullptr;
}

} // namespace sunrise::steam::interfaces
