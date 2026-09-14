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
inline constexpr std::size_t kCapacity = kRootsPerFamily * 7;
static_assert(kCapacity <= 256);
struct Subscription {
    std::uint64_t root{};
    std::uint64_t character{};
    std::uint64_t nextAttemptTick{};
    std::uint32_t generation{};
    std::int32_t version{};
    std::uint8_t family{};
    bool hasFrame{};
    bool replayPending{};
};
struct Subscriptions {
    std::array<Subscription, kCapacity> entries{};
    std::uint8_t cursor{};
};
enum class Result { notHandled, success, failure };

/** Public subscriptions are recipient/root scoped and never use the local investment ladder. */
[[nodiscard]] Result consume(Session& session,
                             Scratch& scratch,
                             const middleware::bap::RequestFrame& request,
                             std::span<std::byte> response,
                             std::size_t& written,
                             std::uint64_t now) noexcept;
[[nodiscard]] bool poll(Session& session,
                        Scratch& scratch,
                        std::span<std::byte> response,
                        std::size_t& written,
                        bool& touchesScratch,
                        std::uint64_t now) noexcept;
} // namespace sunrise::server::bap::encrypted::public_queuez
