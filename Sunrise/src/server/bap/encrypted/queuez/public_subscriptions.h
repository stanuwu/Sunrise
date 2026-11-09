#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../middleware/bap/frame.h"

namespace sunrise::server::bap {
struct Session;
struct Scratch;
} // namespace sunrise::server::bap
namespace sunrise::server::bap::encrypted::public_queuez {

/** Each supported public family has its own bounded root table. */
inline constexpr std::size_t kRootsPerFamily = 32;
/** Public families a session may declare, which is every generated family except unlocks. */
inline constexpr std::size_t kSupportedFamilyCount = 7;
/** Total declared-subscription slots across every supported family. */
inline constexpr std::size_t kCapacity = kRootsPerFamily * kSupportedFamilyCount;
/** The update cursor below is one byte wide, so the table may not outgrow what it can index. */
static_assert(kCapacity <= 256);
/** One session's declared subscription to a public root. */
struct Subscription {
    std::uint64_t root{};
    std::uint64_t character{};
    std::uint32_t generation{};
    std::int32_t version{};
    std::uint8_t family{};
};
/** A session's declared subscriptions, polled round-robin from `cursor`. */
struct Subscriptions {
    std::array<Subscription, kCapacity> entries{};
    std::uint8_t cursor{};
};
/** Outcome of `consume`: not one of this route's requests, decoded and applied, or refused. */
enum class Result { notHandled, success, failure };

/**
 * Answers a subscribe, unsubscribe or explicit WS-206 family fetch against public profiles.
 *
 * Every accepted subscribe/fetch returns a snapshot and retains a subscription until unsubscribe
 * or disconnect. Refused WS-206 fetches receive a correlated status-only reply and add no root.
 * Output-capacity failures commit neither nonce nor subscription.
 *
 * Publication versions are recipient/root scoped and never use the local investment ladder.
 */
[[nodiscard]] Result consume(Session& session,
                             Scratch& scratch,
                             const middleware::bap::RequestFrame& request,
                             std::span<std::byte> response,
                             std::size_t& written) noexcept;
/**
 * Sends the next due subscription update, advancing the round-robin cursor as it scans. False
 * once a full pass finds nothing due, or on an encode failure.
 */
[[nodiscard]] bool poll(Session& session,
                        Scratch& scratch,
                        std::span<std::byte> response,
                        std::size_t& written,
                        bool& touchesScratch) noexcept;
} // namespace sunrise::server::bap::encrypted::public_queuez
