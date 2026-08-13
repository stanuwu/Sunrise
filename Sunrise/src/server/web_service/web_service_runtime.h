#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/web_service/messages/opcode206.h"

namespace sunrise::server::web_service {

/** Optional Server action produced while answering one Web Service request. */
struct Outcome {
    bool hasSubscription{};
    middleware::queuez::Subscription subscription{};
    /** An opcode-504 pick moved the selection and its Family-4 object still has to follow. */
    bool hasSelectedCharacter{};
    std::uint64_t selectedCharacterSoid{};
    /** A collection withdrawal changed the selected character's inventory. */
    bool hasInventoryMutation{};
    std::uint64_t acquiredInstanceSoid{};
};

/**
 * Answers one whole supported Web Service request body.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Answers one request and reports any subscription side effect.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written,
                           Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
