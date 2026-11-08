#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80804e.h"
#include "../../content/packages/schema/generated/tag_classes_808050.h"
#include "auth_fields.h"

namespace sunrise::middleware::bap::activity_message::map_generator_auth {

namespace generated = middleware::content::packages::schema::generated;

/** Type-37 Auth contains two generator records and one retained state block. */
inline constexpr std::uint8_t kSlotType = 37;
inline constexpr std::uint32_t kComponentClass = generated::C80804ef6::kClass;
inline constexpr std::uint32_t kSchema = generated::C80805007::kClass;
inline constexpr std::size_t kRecordCount = 2;
inline constexpr std::size_t kAnchorCount = 4;
inline constexpr std::size_t kIntegerCount = 5;
inline constexpr std::size_t kRealCount = 2;
inline constexpr std::size_t kAreaCount = 32;
inline constexpr std::size_t kGatewayCount = 64;
/** Each no-tile record occupies 475 bits; the state block occupies 800 bits. */
inline constexpr std::size_t kRecordBits = 475;
inline constexpr std::size_t kStateBits = 800;
inline constexpr std::size_t kBitCount = kRecordCount * kRecordBits + kStateBits;
inline constexpr std::size_t kByteCount = (kBitCount + 7U) / 8U;
/** Signed byte fields use bias 128; masks and tile counts have seven bits. */
inline constexpr std::uint8_t kByteWidth = 8;
inline constexpr std::uint8_t kWordWidth = 32;
inline constexpr std::uint32_t kByteBias = 128;
inline constexpr std::uint8_t kMaskWidth = 7;
inline constexpr std::uint8_t kTileCountWidth = 7;
inline constexpr std::uint8_t kMaximumMask = 0x7FU;
/** Unselected inputs keep the authored values through their masks or their -1 sentinels. */
inline constexpr std::int32_t kAuthoredInteger = -1;
inline constexpr float kAuthoredReal = -1.F;

/** Each bit selects one field group instead of the worker's authored value. */
enum class Override : std::uint8_t {
    seed = 0x01,
    mode = 0x02,
    anchors = 0x04,
    enabled = 0x08,
    integer2 = 0x10,
    integer3 = 0x20,
    integer4 = 0x40,
};

/** Two authored grid selectors, a float input and an enable flag form each anchor. */
struct Anchor final {
    std::int8_t first{-1};
    std::int8_t second{-1};
    float value{};
    bool enabled{};
};

/** Unknown worker inputs remain explicit native values until their SDK meaning is established. */
struct Record final {
    std::uint32_t seed{};
    std::int8_t mode{};
    std::array<Anchor, kAnchorCount> anchors{};
    std::uint8_t overrides{};
    bool enabled{};
    std::array<float, kRealCount> realInputs{kAuthoredReal, kAuthoredReal};
    std::array<std::int32_t, kIntegerCount> integerInputs{
        kAuthoredInteger, kAuthoredInteger, kAuthoredInteger, kAuthoredInteger, kAuthoredInteger};
};

/** The state key qualifies the area and gateway byte banks. */
struct Body final {
    std::array<Record, kRecordCount> records{};
    std::uint32_t stateKey{};
    std::array<std::uint8_t, kAreaCount> areas{};
    std::array<std::uint8_t, kGatewayCount> gateways{};
};

/** @return True when every authored float and the override mask fits the native record. */
[[nodiscard]] inline bool valid(const Record& record) noexcept {
    if (record.overrides > kMaximumMask) {
        return false;
    }
    for (const auto& anchor : record.anchors) {
        if (!std::isfinite(anchor.value)) {
            return false;
        }
    }
    for (const float value : record.realInputs) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

/** Writes a signed byte with its native bias. */
[[nodiscard]] inline bool write_byte(encoding::bits::Writer& writer, std::int8_t value) noexcept {
    return writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)
                                                   + static_cast<std::int32_t>(kByteBias)),
                        kByteWidth);
}

/** Writes one record with no host-authored tiles. */
[[nodiscard]] inline bool write_record(encoding::bits::Writer& writer,
                                       const Record& record) noexcept {
    bool encoded = writer.write(record.seed, kWordWidth) && write_byte(writer, record.mode);
    for (const Anchor& anchor : record.anchors) {
        encoded = encoded && write_byte(writer, anchor.first) && write_byte(writer, anchor.second)
                  && writer.write(std::bit_cast<std::uint32_t>(anchor.value), kWordWidth)
                  && writer.write(anchor.enabled, auth_fields::kBoolWidth);
    }
    encoded = encoded && writer.write(record.overrides, kMaskWidth)
              && writer.write(record.enabled, auth_fields::kBoolWidth);
    for (const float value : record.realInputs) {
        encoded = encoded && writer.write(std::bit_cast<std::uint32_t>(value), kWordWidth);
    }
    for (const std::int32_t value : record.integerInputs) {
        encoded = encoded
                  && writer.write(std::bit_cast<std::uint32_t>(value) + auth_fields::kSigned32Bias,
                                  kWordWidth);
    }
    return encoded && writer.write(0, kTileCountWidth);
}

/** Writes the complete no-tile body into an enclosing sensor message. */
[[nodiscard]] inline bool write_body(encoding::bits::Writer& writer, const Body& body) noexcept {
    for (const Record& record : body.records) {
        if (!valid(record)) {
            return false;
        }
    }
    for (const Record& record : body.records) {
        if (!write_record(writer, record)) {
            return false;
        }
    }
    if (!writer.write(body.stateKey, kWordWidth)) {
        return false;
    }
    for (const std::uint8_t value : body.areas) {
        if (!writer.write(value, kByteWidth)) {
            return false;
        }
    }
    for (const std::uint8_t value : body.gateways) {
        if (!writer.write(value, kByteWidth)) {
            return false;
        }
    }
    return true;
}

// TODO: expose generator controls after the SDK binds their authored worker inputs.
/** Encodes a complete no-tile generator body. */
[[nodiscard]] inline bool
encode(const Body& body, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kByteCount) {
        return false;
    }
    encoding::bits::Writer writer(output.first(kByteCount));
    return write_body(writer, body)
           && auth_fields::finish_exact(writer, kBitCount, kByteCount, written);
}

} // namespace sunrise::middleware::bap::activity_message::map_generator_auth
