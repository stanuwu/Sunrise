#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80809c.h"
#include "definition.h"

namespace sunrise::middleware::bap::activity_message::sensor_message {

namespace generated = middleware::content::packages::schema::generated;

/** Message type and the schemas the client accepts for the sensor envelope. */
inline constexpr std::uint32_t kMessageType = 7;
inline constexpr std::uint32_t kClientReferenceSchema = generated::ClientRef::kClass;
/** All-ones names no selected body; the client then reads only the short header. */
inline constexpr std::uint32_t kAbsentSchema = 0xFFFFFFFFU;
/** Header sizes with and without a selected body, in bits then bytes. */
inline constexpr std::size_t kAbsentHeaderBits = 56;
inline constexpr std::size_t kPresentHeaderBits = 88;
inline constexpr std::size_t kAbsentByteCount = kAbsentHeaderBits / 8U;
inline constexpr std::size_t kPresentHeaderBytes = kPresentHeaderBits / 8U;
/** Body bits left once the header is written into the largest payload. */
inline constexpr std::size_t kMaximumSelectedBodyBits =
    kMaximumPayloadSize * 8U - kPresentHeaderBits;

/** Exact logical fields in the fixed 55-bit client-reference prefix. */
struct ClientReference final {
    std::uint32_t rosterKey{};
    std::int8_t slotType{-1};
    std::int16_t slotIndex{-1};
};

/** Structural message-7 envelope with an MSB-first runtime-selected body view. */
struct Body final {
    ClientReference target{};
    std::uint32_t schema{kAbsentSchema};
    std::span<const std::byte> selectedBody{};
    std::size_t selectedBodyBits{};
    bool schemaPresent{};
};

/** Computes the meaningful and padded extents for a schema-present body. */
[[nodiscard]] bool encoded_extent(std::size_t selectedBodyBits,
                                  std::size_t& meaningfulBits,
                                  std::size_t& bytes) noexcept;

/** Encodes a schema-present envelope. The 56-bit schema-absent form is decode-only. */
[[nodiscard]] bool encode(const Body& body,
                          std::span<std::byte> output,
                          std::size_t& written,
                          std::size_t& writtenBits) noexcept;

/** Decodes one exact envelope using the selected schema's known body bit extent. */
[[nodiscard]] bool
decode(std::span<const std::byte> input, std::size_t selectedBodyBits, Body& body) noexcept;

/** Checks one exact envelope without retaining its borrowed selected-body view. */
[[nodiscard]] bool validate(std::span<const std::byte> input,
                            std::size_t selectedBodyBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sensor_message
