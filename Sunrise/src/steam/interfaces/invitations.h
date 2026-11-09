#pragma once

#include <array>
#include <cstdint>

#include "../../state/social/social_feed.h"

namespace sunrise::steam::interfaces::methods {
/** One invitation offered for accept or decline through `pending_invitation`. */
struct PendingInvitation {
    std::uint64_t id{};
    std::uint64_t inviterSteamId{};
    std::array<char, state::social::feed::kNameCapacity> inviterName{};
};
/** Reads the published invitation without entering producer or server locks. */
[[nodiscard]] bool pending_invitation(PendingInvitation& output) noexcept;
/** Posts the first choice for a presented ID; the callback pump revalidates it before delivery. */
[[nodiscard]] bool decide_invitation(std::uint64_t id, bool accept) noexcept;
} // namespace sunrise::steam::interfaces::methods
