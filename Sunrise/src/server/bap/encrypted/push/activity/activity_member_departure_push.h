#pragma once

#include "../../../../../state/activity/member_departure.h"
#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
/** Stages every missing sequential epoch, retaining an optional departure purge at its step. */
[[nodiscard]] bool append_replication_steps(Session& session,
                                            Scratch& scratch,
                                            std::uint64_t target,
                                            const state::activity::MemberPurge* purge,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::array<std::byte, state::kBapNonceSize>& nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written) noexcept;
/** Updates the connection and server peer only after its complete ordered frame is published. */
std::size_t commit_replication_steps(Session& session, std::uint64_t target) noexcept;
/** Publishes one owed pure step without waiting for the periodic keepalive. */
[[nodiscard]] bool consume_replication_step(Session& session,
                                            Scratch& scratch,
                                            std::span<std::byte> response,
                                            std::size_t& written,
                                            bool& touchesScratch) noexcept;
/** Publishes this recipient's retired entity slots before its next membership update. */
[[nodiscard]] bool consume_member_departure(Session& session,
                                            Scratch& scratch,
                                            std::span<std::byte> response,
                                            std::size_t& written,
                                            bool& touchesScratch) noexcept;
/** Replays the recipient's own join result once this link delivered the changed membership. */
[[nodiscard]] bool consume_member_rejoin(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept;
} // namespace sunrise::server::bap::encrypted::push::activity
