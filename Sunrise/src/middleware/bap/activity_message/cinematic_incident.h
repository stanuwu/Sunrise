#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808087.h"

namespace sunrise::middleware::bap::activity_message::cinematic_incident {

namespace generated = middleware::content::packages::schema::generated;

/** Global SObject row emitted after a cinematic was accepted by the local runtime. */
inline constexpr std::uint32_t kStartedTarget = 5'239;
/** Global SObject row emitted for start failure or when the runtime stops owning the resource. */
inline constexpr std::uint32_t kTerminatedTarget = 1'685;
/** Global SObject row emitted when the player asks to skip a cinematic. */
inline constexpr std::uint32_t kSkipTarget = 3'338;
/** Effective type-17 incident metadata schema selected by the three cinematic SObject rows. */
inline constexpr std::uint32_t kSchema = generated::C808087bf::kClass;
/** The native schema consumes 486 meaningful bits and two zero padding bits. */
inline constexpr std::size_t kPayloadBits = 486;
inline constexpr std::size_t kPayloadBytes = 61;

enum class Signal : std::uint8_t {
    started,
    terminated,
    skipRequested,
};

/** Exact Type-6 source ClientRef and runtime values retained from one cinematic incident. */
struct Payload final {
    std::uint32_t registryKey{};
    std::uint64_t runtimeObjectId{};
    float eventValue{};
    std::int16_t slotIndex{-1};
    std::int8_t slotType{-1};
};

/** Maps one exact global SObject row to its cinematic signal. */
[[nodiscard]] bool signal_for_target(std::uint32_t target, Signal& output) noexcept;

/** Decodes the fixed kSchema payload, including its required zero padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Payload& output) noexcept;

} // namespace sunrise::middleware::bap::activity_message::cinematic_incident
