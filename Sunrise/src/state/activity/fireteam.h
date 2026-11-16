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
/** Reads whether the account's own join lookup still waits for native admission. */
[[nodiscard]] bool joining(std::uint64_t account) noexcept;
/** Disconnect or an authenticated native solo split leaves the activity simulation intact. */
[[nodiscard]] bool depart(std::uint64_t account) noexcept;
/**
 * Detects the same character moving to a new solo group out of a fireteam, whether its previous
 * upload named that fireteam or only the server holds it there. The client uploads its presence
 * when its join descriptor changes, and joining someone else's group changes only the joiner's,
 * so the one who was joined can leave before any upload of theirs counted the joiner.
 * @param account Primary account the presence belongs to.
 */
[[nodiscard]] bool native_solo_split(std::uint64_t account,
                                     const social::NativePresence& before,
                                     const social::NativePresence& after) noexcept;
} // namespace sunrise::state::activity::fireteam
