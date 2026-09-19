#pragma once

#include <cstdint>

#include "../social/native_presence.h"

namespace sunrise::state::activity::fireteam {
/** A successful native join-directory lookup records intent, never membership or readiness. */
[[nodiscard]] bool request_join(std::uint64_t joiner, std::uint64_t target) noexcept;
/** Reads established connectivity under the State lock; pending join intent is excluded. */
[[nodiscard]] bool connected(std::uint64_t first, std::uint64_t second) noexcept;
/** Minimum account key in an established multi-account component, or zero when unconnected. */
[[nodiscard]] std::uint64_t representative(std::uint64_t account) noexcept;
/** Reads the membership revision under the State lock; recording intent does not advance it. */
[[nodiscard]] std::uint64_t revision() noexcept;
/** Disconnect or an authenticated native solo split leaves the activity simulation intact. */
[[nodiscard]] bool depart(std::uint64_t account) noexcept;
/** Detects the same character moving from a published multi-member group to a new solo group. */
[[nodiscard]] bool native_solo_split(const social::NativePresence& before,
                                     const social::NativePresence& after) noexcept;
} // namespace sunrise::state::activity::fireteam
