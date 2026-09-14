#pragma once

#include <array>
#include <cstdint>

namespace sunrise::steam::interfaces::methods {
struct PendingInvitation {
    std::uint64_t id{};
    std::uint64_t inviterSteamId{};
    std::array<char, 256> inviterName{};
};
[[nodiscard]] bool pending_invitation(PendingInvitation& output) noexcept;
/** Records a decision for this invitation; callback delivery stays on the native callback pump. */
[[nodiscard]] bool decide_invitation(std::uint64_t id, bool accept) noexcept;
} // namespace sunrise::steam::interfaces::methods
