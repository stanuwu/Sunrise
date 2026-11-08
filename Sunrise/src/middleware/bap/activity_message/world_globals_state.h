#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808086.h"

namespace sunrise::middleware::bap::activity_message::world_globals_state {

namespace generated = middleware::content::packages::schema::generated;

/** Message type and schema the client accepts, and the fixed size of its body. */
inline constexpr std::uint32_t kMessageType = 2;
inline constexpr std::uint32_t kSchema = generated::C8080867e::kClass;
inline constexpr std::size_t kBitCount = 33;
inline constexpr std::size_t kByteCount = 5;

/** Complete fixed message-2 body, with the real32 retained as raw IEEE-754 bits. */
struct Body final {
    bool flag{};
    std::uint32_t real32Bits{};
};

/** Encodes the exact bool followed by the unaligned 32-bit real value. */
[[nodiscard]] bool
encode(const Body& body, std::span<std::byte> output, std::size_t& written) noexcept;

/** Decodes one exact five-byte body and rejects nonzero trailing padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Body& body) noexcept;

/** Checks one exact message-2 body without retaining its decoded state. */
[[nodiscard]] bool validate(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::activity_message::world_globals_state
