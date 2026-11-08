#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808087.h"

namespace sunrise::middleware::bap::activity_message::player_trigger_incident {

namespace generated = middleware::content::packages::schema::generated;

/** Global incident-table row whose payload uses kSchema. */
inline constexpr std::uint32_t kPrimaryTarget = 6'685;
/** Runtime metadata schema carried by the player-trigger incident payload. */
inline constexpr std::uint32_t kSchema = generated::C8080879f::kClass;
/** The native schema consumes 422 meaningful bits and two zero padding bits. */
inline constexpr std::size_t kPayloadBits = 422;
inline constexpr std::size_t kPayloadBytes = 53;

/** The authored type-31 source ClientRef and resolved target id retained from one payload. */
struct Payload final {
    std::uint32_t registryKey{};
    std::uint32_t resolvedObjectId{};
    std::int16_t slotIndex{-1};
    std::int8_t slotType{-1};
};

/** Decodes the fixed kSchema payload, including its required zero padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Payload& output) noexcept;

} // namespace sunrise::middleware::bap::activity_message::player_trigger_incident
