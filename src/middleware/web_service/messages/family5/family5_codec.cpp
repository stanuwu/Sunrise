#include "family5_codec.h"

#include <limits>

namespace sunrise::middleware::web_service::messages::family5 {
namespace {

/** Every optional field and override member has one presence bit next to it. */
constexpr std::uint8_t kPresenceWidth = 1;
/** The nested family object has 10 optional fields in descriptor order. */
constexpr std::size_t kNestedFieldCount = 10;
/** Family object ids are 64 wire bits. */
constexpr std::uint8_t kSoidWidth = 64;
/** Override list counts are unsigned 7-bit fields. */
constexpr std::uint8_t kOverrideCountWidth = 7;
/** Override slots use signed 16-bit descriptors biased into the wire range. */
constexpr std::uint8_t kOverrideSlotWidth = 16;
/** Signed override slots add this descriptor bias before encoding. */
constexpr std::uint32_t kOverrideSlotBias = 0x8000;
/** Logical unlock flags are 2 bits after their descriptor bias. */
constexpr std::uint8_t kFlagValueWidth = 2;
/** Unlock flag values add 1, so logical value 2 stores as 3. */
constexpr std::uint8_t kFlagValueBias = 1;
/**
 * Last flag slot the Client's evaluated-state buffer holds.
 * The Client uses the slot as a raw offset with no bound check, so a later row writes past that
 * buffer. The row is dropped, never the message.
 */
constexpr std::uint16_t kMaximumFlagSlot = 23499;
/** Last value slot the Client's evaluated-state buffer holds. Same raw-offset hazard. */
constexpr std::uint16_t kMaximumValueSlot = 15499;
/** Logical flag values run 0 through 2, inclusive. */
constexpr std::uint8_t kMaximumFlagValue = 2;
/** Signed unlock values fill one 32-bit field. */
constexpr std::uint8_t kUnlockValueWidth = 32;
/** The optional content-gate arm is a 32-bit mask holding bit 0. */
constexpr std::uint8_t kContentGateArmWidth = 32;
/** Bit 0 arms the content-gate shortcut when its second State term is present. */
constexpr std::uint32_t kContentGateArmValue = 1;

/** Descriptor positions for the family-5 fields Sunrise can produce from State. */
enum class NestedField : std::size_t {
    objectSoid = 0,
    flagOverrides = 4,
    valueOverrides = 5,
    contentGateArm = 7,
};

/** @return Flag rows whose slot the Client can address. */
[[nodiscard]] std::size_t safe_flag_count(const state::Family5State& family) noexcept {
    std::size_t safe = 0;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const auto& row = family.flags[index];
        if (row.slot <= kMaximumFlagSlot && row.value <= kMaximumFlagValue) {
            ++safe;
        }
    }
    return safe;
}

/** @return Value rows whose slot the Client can address. */
[[nodiscard]] std::size_t safe_value_count(const state::Family5State& family) noexcept {
    std::size_t safe = 0;
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot <= kMaximumValueSlot) {
            ++safe;
        }
    }
    return safe;
}

/**
 * Writes a nonempty flag-override list after its outer presence bit.
 * The count covers only the rows written, so a dropped row leaves no gap.
 * @param writer Fixed-buffer payload writer.
 * @return True when the whole list fits.
 */
[[nodiscard]] bool write_flags(encoding::bits::Writer& writer,
                               const state::Family5State& family) noexcept {
    bool encoded = writer.write(safe_flag_count(family), kOverrideCountWidth);
    for (std::size_t index = 0; encoded && index < family.flagCount; ++index) {
        const auto& row = family.flags[index];
        if (row.slot > kMaximumFlagSlot || row.value > kMaximumFlagValue) {
            continue;
        }
        const std::uint32_t storedSlot = row.slot + kOverrideSlotBias;
        const std::uint32_t storedValue = row.value + kFlagValueBias;
        encoded = writer.write(1U, kPresenceWidth) && writer.write(storedSlot, kOverrideSlotWidth)
                  && writer.write(1U, kPresenceWidth) && writer.write(storedValue, kFlagValueWidth);
    }
    return encoded;
}

/**
 * Writes a nonempty signed-value override list after its outer presence bit.
 * The count covers only the rows written, so a dropped row leaves no gap.
 * @param writer Fixed-buffer payload writer.
 * @return True when the whole list fits.
 */
[[nodiscard]] bool write_values(encoding::bits::Writer& writer,
                                const state::Family5State& family) noexcept {
    bool encoded = writer.write(safe_value_count(family), kOverrideCountWidth);
    for (std::size_t index = 0; encoded && index < family.valueCount; ++index) {
        const auto& row = family.values[index];
        if (row.slot > kMaximumValueSlot) {
            continue;
        }
        const std::uint32_t storedSlot = row.slot + kOverrideSlotBias;
        const std::int64_t biased =
            static_cast<std::int64_t>(row.value) - (std::numeric_limits<std::int32_t>::min)();
        encoded = writer.write(1U, kPresenceWidth) && writer.write(storedSlot, kOverrideSlotWidth)
                  && writer.write(1U, kPresenceWidth)
                  && writer.write(static_cast<std::uint32_t>(biased), kUnlockValueWidth);
    }
    return encoded;
}

} // namespace

/** Reports whether the override lists stay inside their own storage. */
bool valid(const state::Family5State& family) noexcept {
    return family.flagCount <= family.flags.size() && family.valueCount <= family.values.size();
}

/** Writes all 10 family-5 fields, each presence bit next to its own data. */
bool write(encoding::bits::Writer& writer, const state::Family5State& family) noexcept {
    bool encoded = true;
    // Presence bits stay interleaved with data so later descriptor fields remain aligned.
    for (std::size_t index = 0; encoded && index < kNestedFieldCount; ++index) {
        const auto field = static_cast<NestedField>(index);
        if (field == NestedField::objectSoid) {
            encoded =
                writer.write(1U, kPresenceWidth) && writer.write(family.objectSoid, kSoidWidth);
        } else if (field == NestedField::flagOverrides && family.flagCount != 0) {
            // An empty list stays absent. A present zero count is a different wire shape.
            encoded = writer.write(1U, kPresenceWidth) && write_flags(writer, family);
        } else if (field == NestedField::valueOverrides && family.valueCount != 0) {
            encoded = writer.write(1U, kPresenceWidth) && write_values(writer, family);
        } else if (field == NestedField::contentGateArm && family.contentGateArm) {
            // A false arm is absent. A present arm always carries the required bit-0 mask.
            encoded = writer.write(1U, kPresenceWidth)
                      && writer.write(kContentGateArmValue, kContentGateArmWidth);
        } else {
            encoded = writer.write(0U, kPresenceWidth);
        }
    }
    return encoded;
}

} // namespace sunrise::middleware::web_service::messages::family5
