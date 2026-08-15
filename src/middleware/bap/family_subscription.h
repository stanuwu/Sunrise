#pragma once

#include <cstddef>
#include <span>

#include "../queuez/subscription.h"

namespace sunrise::middleware::bap::family_subscription {

/**
 * Decodes the fixed authenticated family-subscription selector.
 * @param input Svc-12 body with the family and root prefix.
 * @param subscription Receives the replicated family and the client-chosen root id.
 * @return True when the body has both fixed prefix fields.
 */
[[nodiscard]] bool parse(std::span<const std::byte> input,
                         queuez::Subscription& subscription) noexcept;

} // namespace sunrise::middleware::bap::family_subscription
