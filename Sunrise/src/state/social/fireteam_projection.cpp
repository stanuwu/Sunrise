#include "fireteam_projection.h"

namespace sunrise::state::social {

bool project_fireteam(std::uint64_t accountSoid,
                      std::span<const NativePublication> publications,
                      std::array<std::byte, kNativeFireteamSize>& output) noexcept {
    if (accountSoid == 0) {
        return false;
    }
    const NativePublication* owner{};
    for (const auto& publication : publications) {
        if (publication.primarySoid != accountSoid) {
            continue;
        }
        if (owner) {
            return false;
        }
        owner = &publication;
    }
    if (!owner || !owner->presence.published || !owner->presence.hasFireteam) {
        return false;
    }
    // WS702 and the family-six descriptor embed the same native 808079BE record. Membership,
    // row ordering, occupancy and account tier belong to that record's publisher.
    output = owner->presence.fireteam;
    return true;
}

} // namespace sunrise::state::social
