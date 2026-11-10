#pragma once

#include <array>
#include <cstdint>

#include "../../../core/network_capacity.h"

namespace sunrise::server::gameplay::group::residency {
/** Read outside the admitted-table lock, then applied only to the same native peer identity. */
struct Report {
    std::int32_t sessionRegion{-1};
    std::int32_t pendingRegion{-1};
    std::int32_t currentRegion{-1};
    std::array<std::uint64_t, core::network_capacity::kActivityPlayers> fireteamAccounts{};
    [[nodiscard]] bool connected(std::uint64_t account) const noexcept {
        if (!account) {
            return false;
        }
        for (const auto peer : fireteamAccounts) {
            if (peer == account) {
                return true;
            }
        }
        return false;
    }
    bool operator==(const Report&) const noexcept = default;
};
/** Current native residency keeps the peer while its next region is precaching. */
[[nodiscard]] inline bool member(bool self,
                                 std::int32_t sessionRegion,
                                 std::int32_t pendingRegion,
                                 std::int32_t currentRegion) noexcept {
    return self || sessionRegion < 0 || pendingRegion < 0 || pendingRegion == sessionRegion
           || currentRegion == sessionRegion;
}
/** A fireteam split retains a native player still reporting this public region. */
[[nodiscard]] inline bool player(bool self,
                                 std::uint64_t recipientAccount,
                                 std::uint64_t peerAccount,
                                 std::int32_t sessionRegion,
                                 std::int32_t pendingRegion,
                                 std::int32_t currentRegion,
                                 bool connected) noexcept {
    return self || !recipientAccount || !peerAccount || connected
           || (sessionRegion >= 0
               && (pendingRegion == sessionRegion || currentRegion == sessionRegion));
}
} // namespace sunrise::server::gameplay::group::residency
