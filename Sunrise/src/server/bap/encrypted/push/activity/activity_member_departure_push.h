#pragma once

#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
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
