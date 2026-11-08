#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80807e.h"
#include "sense_update.h"

namespace sunrise::middleware::bap::activity_message::squad_sense {

namespace generated = middleware::content::packages::schema::generated;

/** Native squad Sense schema and its fixed array capacities. */
inline constexpr std::uint32_t kSchema = generated::C80807ecc::kClass;
inline constexpr std::size_t kScalarCount = 6, kCountCapacity = 8, kRealCount = 24;
/** Squad ClientRefs use type one; the largest full Sense body occupies 606 bits. */
inline constexpr std::uint8_t kSlotType = 1;
inline constexpr std::size_t kMaximumByteCount = 76;

/** Presence keeps unknown fields at the client's schema defaults. */
struct OptionalScalar final {
    std::uint32_t raw{};
    bool present{};
};

/** Quantized reals retain their wire values without a float round trip. */
struct State final {
    std::array<OptionalScalar, kScalarCount> scalars{};
    std::array<std::int32_t, kCountCapacity> counts{};
    std::array<OptionalScalar, kRealCount> reals{};
    std::uint32_t counter{};
    std::int8_t field6{};
    std::int8_t field7{};
    std::uint8_t count{};
    bool field8{};
    bool field9{};
    bool initialized{};
    bool countsPresent{};
    bool realsPresent{};
    bool valid{};
};

/** Merges one complete object from the full packet value span; failure leaves state unchanged. */
[[nodiscard]] bool merge(State& state,
                         const sense_update::DecodedObject& object,
                         std::span<const sense_update::DecodedValue> values) noexcept;

/** Encodes a full delta-root body; publication carries the counter separately. */
[[nodiscard]] bool encode(const State& state,
                          std::span<std::byte> output,
                          std::size_t& bytes,
                          std::size_t& bits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::squad_sense
