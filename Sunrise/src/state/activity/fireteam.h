#pragma once

#include <cstdint>

#include "../social/native_presence.h"

namespace sunrise::state::activity::fireteam {
/** A successful native join-directory lookup records intent, never membership or readiness. */
[[nodiscard]] bool
request_join(std::uint64_t joiner, std::uint64_t target, std::uint64_t now) noexcept;
[[nodiscard]] std::uint64_t pending_target(std::uint64_t joiner, std::uint64_t now) noexcept;
[[nodiscard]] bool connected(std::uint64_t first, std::uint64_t second) noexcept;
[[nodiscard]] std::uint64_t representative(std::uint64_t account) noexcept;
[[nodiscard]] std::uint64_t revision() noexcept;
void expire(std::uint64_t now) noexcept;
/** Disconnect or an authenticated native solo split leaves the activity simulation intact. */
[[nodiscard]] bool depart(std::uint64_t account) noexcept;
[[nodiscard]] bool native_solo_split(const social::NativePresence& before,
                                     const social::NativePresence& after) noexcept;
} // namespace sunrise::state::activity::fireteam
