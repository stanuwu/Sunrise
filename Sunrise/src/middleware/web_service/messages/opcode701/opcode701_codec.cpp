#include "opcode701_codec.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../../../state/account/settings/native_key_binding_map.h"
#include "../../../../state/account/settings/settings_state.h"
#include "../../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode701 {
namespace {

using encoding::bits::Reader;
namespace settings = state::account::settings;

/**
 * Schema 0x80807603 is a presence-driven reflected object. Every `optional` node starts with one
 * presence bit; an absent node consumes no body bits. No field is byte-aligned.
 *
 * Implicit root (there is no root presence bit)
 * |-- 0.0? client metadata
 * |   |-- 0.0.0? [128] optional 64-bit publicity expiries
 * |   `-- 0.0.1? [13] required 32-bit seen-message values
 * `-- 0.1? account data
 *     |-- 0.1.0? [2] optional vectors, each with two required real32 values
 *     |-- 0.1.1? preference record
 *     |   |-- 0.1.1.0-.61: 62 optional scalar preferences
 *     |   `-- 0.1.1.62? [3][50]: 150 cells, each with its own presence bit and int32
 *     |-- 0.1.2? seed, three local mirrors, source, and optional 60-word binding table
 *     |-- 0.1.3? four required 16-bit values
 *     |-- 0.1.4? mixed known-width record, semantic meaning unknown
 *     |-- 0.1.5? 22 required 32-bit values
 *     |-- 0.1.6? optional-region record
 *     |   |-- 0.1.6.0? [100] optional int16 values, then two required int32 words
 *     |   `-- 0.1.6.1? one int16 value
 *     |-- 0.1.7? bool
 *     |-- 0.1.8? bool
 *     |-- 0.1.9? bool
 *     |-- 0.1.10? 8-bit scalar
 *     |-- 0.1.11? 32-bit scalar
 *     |-- 0.1.12? 30 required int16 values, then two required int32 words
 *     `-- 0.1.13? one 32-bit value
 *
 * Two optional length-prefixed blobs follow the reflected object. The final partial byte, if any,
 * is zero padding. Traversal must therefore follow every presence flag even for unsupported data;
 * a fixed wire offset would become invalid as soon as any earlier optional node is absent.
 */

/** Wire primitive widths used by schema 0x80807603. */
constexpr std::uint8_t kPresenceWidthBits = 1;
constexpr std::uint8_t kBooleanWidthBits = 1;
constexpr std::uint8_t kTwoWidthBits = 2;
constexpr std::uint8_t kThreeWidthBits = 3;
constexpr std::uint8_t kFourWidthBits = 4;
constexpr std::uint8_t kFiveWidthBits = 5;
constexpr std::uint8_t kSixWidthBits = 6;
constexpr std::uint8_t kByteWidthBits = 8;
constexpr std::uint8_t kScalar16WidthBits = 16;
constexpr std::uint8_t kScalar32WidthBits = 32;
constexpr std::uint8_t kScalar64WidthBits = 64;

/** Schema array dimensions, kept separate from scalar widths. */
constexpr std::size_t kPublicityExpiryCount = 128;
constexpr std::size_t kSeenMessageCount = 13;
constexpr std::size_t kCalibrationVectorCount = 2;
constexpr std::size_t kCalibrationValuesPerVector = 2;
constexpr std::size_t kPreferenceMatrixRowCount = 3;
constexpr std::size_t kPreferenceMatrixColumnCount = 50;
constexpr std::size_t kGroup_0_1_4OptionalFieldCount = 8;
constexpr std::size_t kGroup_0_1_3ValueCount = 4;
constexpr std::size_t kGroup_0_1_5ValueCount = 22;
constexpr std::size_t kGroup_0_1_6EntryCount = 100;
constexpr std::size_t kGroup_0_1_6FixedWordCount = 2;
constexpr std::size_t kGroup_0_1_12ValueCount = 30;
constexpr std::size_t kGroup_0_1_12FixedWordCount = 2;
constexpr std::size_t kOuterBlobCount = 2;

/** Catalog invariants used to detect accidental table/schema drift at compile time. */
constexpr std::size_t kCatalogPreferenceFieldCount = 62;
constexpr std::size_t kCatalogKeyBindingCount = 60;
constexpr std::size_t kCatalogRootMetadataMaximumBits = 8'739;
constexpr std::size_t kCatalogPreferencesMaximumBits = 5'300;
constexpr std::size_t kCatalogBindingsMaximumBits = 1'996;
constexpr std::size_t kCatalogAccountBranchMaximumBits = 10'956;
constexpr std::size_t kCatalogMaximumBits = 19'695;

/** A packed binding half with this value represents no assigned input. */
constexpr std::uint16_t kUnboundInputCode = settings::bindings::kUnboundInputCode;
/** Each packed binding word stores one primary half followed by one secondary half. */
constexpr unsigned kBindingHalfWidthBits = kScalar16WidthBits;

/** Outer blobs encode their byte length in one unsigned 16-bit field. */
constexpr std::uint8_t kOuterBlobLengthWidthBits = kScalar16WidthBits;
/** A whole unread byte is data, while fewer than eight final bits may be terminal padding. */
constexpr std::size_t kTerminalPaddingLimitBits = kByteWidthBits;

/** Returns the bit count of a required fixed-width array. */
[[nodiscard]] constexpr std::size_t fixed_array_width_bits(std::size_t count,
                                                           std::size_t widthBits) noexcept {
    return count * widthBits;
}

/** One scalar descriptor's stored width, destination width, and modular wire bias. */
struct ScalarEncoding {
    std::uint8_t wireWidthBits;
    std::uint8_t nativeWidthBits;
    std::uint64_t bias;
};

/** Compact signed selectors store the destination value plus one. */
constexpr std::uint64_t kCompactIntegerBias = 1;
/** Reflected signed 32-bit values store the destination bit pattern plus INT32_MIN. */
constexpr std::uint64_t kSigned32Bias = 0x80000000ULL;

constexpr ScalarEncoding kBoolEncoding{kBooleanWidthBits, kBooleanWidthBits, 0};
constexpr ScalarEncoding kInt8TwoBitEncoding{kTwoWidthBits, kByteWidthBits, kCompactIntegerBias};
constexpr ScalarEncoding kInt8ThreeBitEncoding{
    kThreeWidthBits, kByteWidthBits, kCompactIntegerBias};
constexpr ScalarEncoding kInt8FourBitEncoding{kFourWidthBits, kByteWidthBits, kCompactIntegerBias};
constexpr ScalarEncoding kInt32Encoding{kScalar32WidthBits, kScalar32WidthBits, kSigned32Bias};
constexpr ScalarEncoding kReal32Encoding{kScalar32WidthBits, kScalar32WidthBits, 0};

constexpr std::size_t kPreferenceFieldCount = kCatalogPreferenceFieldCount;
constexpr std::size_t kKeyBindingCount = settings::bindings::kActionCount;

static_assert(kKeyBindingCount == kCatalogKeyBindingCount);

/** Reinterprets an already-unbiased byte pattern as its signed destination value. */
[[nodiscard]] constexpr std::int8_t as_int8(std::uint64_t value) noexcept {
    return std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(value));
}

/** Reinterprets an already-unbiased 32-bit pattern as its signed destination value. */
[[nodiscard]] constexpr std::int32_t as_int32(std::uint64_t value) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

/** Reinterprets a raw IEEE-754 32-bit pattern without applying an integer conversion. */
[[nodiscard]] constexpr float as_real32(std::uint64_t value) noexcept {
    return std::bit_cast<float>(static_cast<std::uint32_t>(value));
}

/** Typed assignment adapters let each schema descriptor name its exact nested delta member. */
template <auto GroupMember, auto FieldMember>
void assign_bool(std::uint64_t value, settings::SettingsDelta& delta) noexcept {
    (delta.*GroupMember).*FieldMember = value != 0;
}

template <auto GroupMember, auto FieldMember>
void assign_int8(std::uint64_t value, settings::SettingsDelta& delta) noexcept {
    (delta.*GroupMember).*FieldMember = as_int8(value);
}

template <auto GroupMember, auto FieldMember>
void assign_int32(std::uint64_t value, settings::SettingsDelta& delta) noexcept {
    (delta.*GroupMember).*FieldMember = as_int32(value);
}

template <auto GroupMember, auto FieldMember>
void assign_real32(std::uint64_t value, settings::SettingsDelta& delta) noexcept {
    (delta.*GroupMember).*FieldMember = as_real32(value);
}

/** A plain function pointer keeps the descriptor table constexpr and allocation-free. */
using PreferenceAssignment = void (*)(std::uint64_t, settings::SettingsDelta&) noexcept;

/** One schema path's index, wire decoding rule, and optional semantic State destination. */
struct PreferenceDescriptor {
    std::size_t schemaIndex;
    ScalarEncoding encoding;
    PreferenceAssignment assign;
};

/**
 * Single source of truth for preference paths 0.1.1.0 through 0.1.1.61.
 * A null assignment marks a structurally known field that is intentionally traversal-only.
 */
constexpr std::array<PreferenceDescriptor, kPreferenceFieldCount> kPreferenceDescriptors{
    PreferenceDescriptor{0, kBoolEncoding, nullptr},  // profile setup marker
    PreferenceDescriptor{1, kInt32Encoding, nullptr}, // post-processing seed version
    PreferenceDescriptor{
        2,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::controls, &settings::ControlsDelta::buttonLayout>},
    PreferenceDescriptor{
        3,
        kInt8ThreeBitEncoding,
        assign_int8<&settings::SettingsDelta::controls, &settings::ControlsDelta::movementMode>},
    PreferenceDescriptor{4,
                         kInt8FourBitEncoding,
                         assign_int8<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerLookSensitivity>},
    PreferenceDescriptor{5,
                         kInt8ThreeBitEncoding,
                         assign_int8<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::doublePressDelay>},
    PreferenceDescriptor{6,
                         kInt32Encoding,
                         assign_int32<&settings::SettingsDelta::controls,
                                      &settings::ControlsDelta::mouseLookSensitivity>},
    PreferenceDescriptor{7,
                         kReal32Encoding,
                         assign_real32<&settings::SettingsDelta::controls,
                                       &settings::ControlsDelta::adsSensitivityModifier>},
    PreferenceDescriptor{
        8,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::subtitlesMode>},
    PreferenceDescriptor{
        9,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::textSize>},
    PreferenceDescriptor{
        10,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::textColor>},
    PreferenceDescriptor{11,
                         kInt8FourBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::textBackgroundStyle>},
    PreferenceDescriptor{12,
                         kInt8FourBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::textBackgroundOpacity>},
    PreferenceDescriptor{13,
                         kInt8FourBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::reservedTextMode>},
    PreferenceDescriptor{14,
                         kInt8FourBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::subtitleOptionsEntry>},
    PreferenceDescriptor{
        15,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::voiceOutputMode>},
    PreferenceDescriptor{
        16,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::teamVoiceChannel>},
    PreferenceDescriptor{
        17,
        kInt8ThreeBitEncoding,
        assign_int8<&settings::SettingsDelta::display, &settings::DisplayDelta::brightness>},
    PreferenceDescriptor{
        18,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::helmetMode>},
    PreferenceDescriptor{19,
                         kInt8ThreeBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::colorblindMode>},
    PreferenceDescriptor{
        20,
        kInt8ThreeBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::reticleColor>},
    PreferenceDescriptor{
        21,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::reservedMode>},
    PreferenceDescriptor{22, kInt8ThreeBitEncoding, nullptr}, // unmapped audio-padding field
    PreferenceDescriptor{
        23,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::migrationVersion>},
    PreferenceDescriptor{
        24,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::soundEffectsVolume>},
    PreferenceDescriptor{
        25,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::dialogueVolume>},
    PreferenceDescriptor{
        26,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::musicVolume>},
    PreferenceDescriptor{
        27,
        kInt8FourBitEncoding,
        assign_int8<&settings::SettingsDelta::audio, &settings::AudioDelta::chatVolume>},
    PreferenceDescriptor{
        28,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::audio, &settings::AudioDelta::muteWhenUnfocused>},
    PreferenceDescriptor{29,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerInvertVertical>},
    PreferenceDescriptor{30,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerInvertHorizontal>},
    PreferenceDescriptor{31,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::mouseInvertVertical>},
    PreferenceDescriptor{32,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::mouseInvertHorizontal>},
    PreferenceDescriptor{33,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerAutoLookCentering>},
    PreferenceDescriptor{34,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::social,
                                     &settings::SocialDelta::preferGoodConnection>},
    PreferenceDescriptor{35,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerVibration>},
    PreferenceDescriptor{36,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::unidentifiedToggle>},
    PreferenceDescriptor{37,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::mouseAimSmoothing>},
    PreferenceDescriptor{38,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::controls,
                                     &settings::ControlsDelta::controllerSwapShoulders>},
    PreferenceDescriptor{39, kBoolEncoding, nullptr}, // first unmapped identity-padding field
    PreferenceDescriptor{40, kBoolEncoding, nullptr}, // second unmapped identity-padding field
    PreferenceDescriptor{
        41,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::social, &settings::SocialDelta::showRealNames>},
    PreferenceDescriptor{
        42,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::interface, &settings::InterfaceDelta::displayHints>},
    PreferenceDescriptor{
        43,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::display, &settings::DisplayDelta::showFps>},
    PreferenceDescriptor{44,
                         kInt8TwoBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::reticleLocation>},
    PreferenceDescriptor{45,
                         kBoolEncoding,
                         assign_bool<&settings::SettingsDelta::social,
                                     &settings::SocialDelta::clanInviteNotifications>},
    PreferenceDescriptor{
        46,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::social, &settings::SocialDelta::profanityFilter>},
    PreferenceDescriptor{47,
                         kInt8ThreeBitEncoding,
                         assign_int8<&settings::SettingsDelta::interface,
                                     &settings::InterfaceDelta::backgroundOpacity>},
    PreferenceDescriptor{
        48,
        kInt8ThreeBitEncoding,
        assign_int8<&settings::SettingsDelta::interface, &settings::InterfaceDelta::hudOpacity>},
    PreferenceDescriptor{
        49,
        kBoolEncoding,
        assign_bool<&settings::SettingsDelta::social, &settings::SocialDelta::voiceChatEnabled>},
    PreferenceDescriptor{
        50,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::whisperChatMode>},
    PreferenceDescriptor{
        51,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::teamChatJoinMode>},
    PreferenceDescriptor{
        52,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::localChatJoinMode>},
    PreferenceDescriptor{
        53,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::clanChatJoinMode>},
    PreferenceDescriptor{
        54,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::display, &settings::DisplayDelta::hdrMode>},
    PreferenceDescriptor{55,
                         kReal32Encoding,
                         assign_real32<&settings::SettingsDelta::display,
                                       &settings::DisplayDelta::calibrationPrimary>},
    PreferenceDescriptor{56,
                         kReal32Encoding,
                         assign_real32<&settings::SettingsDelta::display,
                                       &settings::DisplayDelta::calibrationAlpha>},
    PreferenceDescriptor{
        57,
        kInt8ThreeBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::textChatMode>},
    PreferenceDescriptor{
        58,
        kInt8TwoBitEncoding,
        assign_int8<&settings::SettingsDelta::social, &settings::SocialDelta::chatAutoHideMode>},
    PreferenceDescriptor{59, kBoolEncoding, nullptr}, // motion-blur mirror
    PreferenceDescriptor{60, kBoolEncoding, nullptr}, // film-grain mirror
    PreferenceDescriptor{61, kBoolEncoding, nullptr}, // chromatic-aberration mirror
};

/** Ensures explicit schema indices stay aligned with descriptor array positions. */
[[nodiscard]] consteval bool valid_preference_descriptors() noexcept {
    for (std::size_t index = 0; index < kPreferenceDescriptors.size(); ++index) {
        const PreferenceDescriptor& descriptor = kPreferenceDescriptors[index];
        if (descriptor.schemaIndex != index || descriptor.encoding.wireWidthBits == 0
            || descriptor.encoding.wireWidthBits > kScalar64WidthBits
            || descriptor.encoding.nativeWidthBits == 0
            || descriptor.encoding.nativeWidthBits > kScalar64WidthBits) {
            return false;
        }
    }
    return true;
}

static_assert(valid_preference_descriptors());

/** Optional widths for unknown mixed record path 0.1.4 fields 0 through 7. */
constexpr std::array<std::uint8_t, kGroup_0_1_4OptionalFieldCount> kGroup_0_1_4OptionalWidths{
    kScalar64WidthBits,
    kScalar64WidthBits,
    kScalar64WidthBits,
    kTwoWidthBits,
    kScalar64WidthBits,
    kTwoWidthBits,
    kSixWidthBits,
    kFiveWidthBits,
};

/** Builds a low-bit mask without evaluating the invalid expression `1 << 64`. */
[[nodiscard]] constexpr std::uint64_t width_mask(std::uint8_t nativeWidthBits) noexcept {
    return nativeWidthBits == kScalar64WidthBits ? (std::numeric_limits<std::uint64_t>::max)()
                                                 : (std::uint64_t{1} << nativeWidthBits) - 1U;
}

/** Reads the one-bit flag that precedes every optional schema node. */
[[nodiscard]] bool read_presence(Reader& reader, bool& present) noexcept {
    std::uint64_t value = 0;
    if (!reader.read(kPresenceWidthBits, value)) {
        return false;
    }
    present = value != 0;
    return true;
}

/** Reads one stored scalar and removes its bias modulo the destination type width. */
[[nodiscard]] bool
read_scalar(Reader& reader, const ScalarEncoding& encoding, std::uint64_t& value) noexcept {
    std::uint64_t stored = 0;
    if (!reader.read(encoding.wireWidthBits, stored)) {
        return false;
    }
    const std::uint64_t mask = width_mask(encoding.nativeWidthBits);
    value = (stored - (encoding.bias & mask)) & mask;
    return true;
}

/** Reads an optional scalar while preserving absent versus present-zero semantics. */
[[nodiscard]] bool read_optional_scalar(Reader& reader,
                                        const ScalarEncoding& encoding,
                                        bool& present,
                                        std::uint64_t& value) noexcept {
    present = false;
    value = 0;
    return read_presence(reader, present) && (!present || read_scalar(reader, encoding, value));
}

/** Consumes one optional field whose value is deliberately not retained. */
[[nodiscard]] bool skip_optional_bits(Reader& reader, std::size_t wireWidthBits) noexcept {
    bool present = false;
    return read_presence(reader, present) && (!present || reader.skip(wireWidthBits));
}

/** Consumes one optional scalar by its declared encoding without retaining its value. */
[[nodiscard]] bool skip_optional_scalar(Reader& reader, const ScalarEncoding& encoding) noexcept {
    return skip_optional_bits(reader, encoding.wireWidthBits);
}

/** Reads one optional group and delegates its body only when the group is present. */
template <typename ReadBody>
[[nodiscard]] bool read_optional_group(Reader& reader, ReadBody readBody) noexcept {
    bool present = false;
    return read_presence(reader, present) && (!present || readBody(reader));
}

/** Consumes the body of optional publicity-expiry bank path 0.0.0. */
[[nodiscard]] bool skip_publicity_expiry_bank(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kPublicityExpiryCount; ++index) {
        if (!skip_optional_bits(reader, kScalar64WidthBits)) {
            return false;
        }
    }
    return true;
}

/** Consumes the fixed seen-message bank at path 0.0.1. */
[[nodiscard]] bool skip_seen_message_bank(Reader& reader) noexcept {
    return reader.skip(fixed_array_width_bits(kSeenMessageCount, kScalar32WidthBits));
}

/** Consumes optional root branch 0.0 in child descriptor order. */
[[nodiscard]] bool skip_publicity_and_seen_messages(Reader& reader) noexcept {
    return read_optional_group(reader, skip_publicity_expiry_bank)
           && read_optional_group(reader, skip_seen_message_bank);
}

/** Consumes one present two-scalar element under calibration path 0.1.0. */
[[nodiscard]] bool skip_calibration_vector(Reader& reader) noexcept {
    return reader.skip(fixed_array_width_bits(kCalibrationValuesPerVector, kScalar32WidthBits));
}

/** Consumes present group 0.1.0, including each element's own presence bit. */
[[nodiscard]] bool skip_group_0_1_0(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kCalibrationVectorCount; ++index) {
        if (!read_optional_group(reader, skip_calibration_vector)) {
            return false;
        }
    }
    return true;
}

/** Consumes present preference matrix path 0.1.1.62 in row-major descriptor order. */
[[nodiscard]] bool skip_preference_matrix(Reader& reader) noexcept {
    for (std::size_t row = 0; row < kPreferenceMatrixRowCount; ++row) {
        for (std::size_t column = 0; column < kPreferenceMatrixColumnCount; ++column) {
            if (!skip_optional_bits(reader, kScalar32WidthBits)) {
                return false;
            }
        }
    }
    return true;
}

/** Decodes present preference group 0.1.1 and consumes its optional opaque matrix. */
[[nodiscard]] bool read_preference_record(Reader& reader, settings::SettingsDelta& delta) noexcept {
    for (const PreferenceDescriptor& descriptor : kPreferenceDescriptors) {
        bool present = false;
        std::uint64_t value = 0;
        if (!read_optional_scalar(reader, descriptor.encoding, present, value)) {
            return false;
        }
        if (present && descriptor.assign != nullptr) {
            descriptor.assign(value, delta);
        }
    }
    return read_optional_group(reader, skip_preference_matrix);
}

/** Decodes the body of present fixed keybinding table path 0.1.2.5 atomically. */
[[nodiscard]] bool read_key_binding_table(Reader& reader, settings::SettingsDelta& delta) noexcept {
    settings::bindings::KeyBindings staged{};
    for (std::size_t nativeSlot = 0; nativeSlot < settings::bindings::kActionsByNativeSlot.size();
         ++nativeSlot) {
        std::uint64_t value = 0;
        if (!read_scalar(reader, kInt32Encoding, value)) {
            return false;
        }

        // After bias removal, bits 0-15 are primary and bits 16-31 are secondary. The value
        // 0x0074 in either half is the protocol's unbound sentinel, not a bindable input.
        const std::uint32_t packed = static_cast<std::uint32_t>(value);
        const std::uint16_t primary = static_cast<std::uint16_t>(packed);
        const std::uint16_t secondary = static_cast<std::uint16_t>(packed >> kBindingHalfWidthBits);
        const auto action = settings::bindings::kActionsByNativeSlot[nativeSlot];
        auto& binding = staged.values[static_cast<std::size_t>(action)];
        if (primary != kUnboundInputCode) {
            binding.primary = primary;
        }
        if (secondary != kUnboundInputCode) {
            binding.secondary = secondary;
        }
    }
    staged.configured = true;
    delta.keyBindings = staged;
    return true;
}

/** Decodes present binding record 0.1.2 in exact child descriptor order. */
[[nodiscard]] bool read_binding_record(Reader& reader, settings::SettingsDelta& delta) noexcept {
    // 0.1.2.0: seed/version marker; structurally consumed but not authoritative.
    if (!skip_optional_scalar(reader, kInt32Encoding)) {
        return false;
    }
    // 0.1.2.1: client-local one-bit mirror; traversal-only.
    if (!skip_optional_scalar(reader, kBoolEncoding)) {
        return false;
    }
    // 0.1.2.2: client-local VSync mirror.
    if (!skip_optional_scalar(reader, kInt8ThreeBitEncoding)) {
        return false;
    }
    // 0.1.2.3: client-local FOV mirror.
    if (!skip_optional_scalar(reader, kInt32Encoding)) {
        return false;
    }

    // 0.1.2.4: authored keybinding source and routing input for the optional table.
    bool sourceSelectorPresent = false;
    std::uint64_t sourceSelector = 0;
    if (!read_optional_scalar(reader, kBoolEncoding, sourceSelectorPresent, sourceSelector)) {
        return false;
    }
    if (sourceSelectorPresent) {
        delta.keyBindingSource = sourceSelector != 0 ? settings::KeyBindingSource::computer
                                                     : settings::KeyBindingSource::account;
    }

    // 0.1.2.5: one presence bit covers the complete 60-entry table.
    return read_optional_group(reader, [&delta](Reader& tableReader) noexcept {
        return read_key_binding_table(tableReader, delta);
    });
}

/** Consumes present fixed-width group 0.1.3. */
[[nodiscard]] bool skip_group_0_1_3(Reader& reader) noexcept {
    return reader.skip(fixed_array_width_bits(kGroup_0_1_3ValueCount, kScalar16WidthBits));
}

/** Consumes present mixed-width group 0.1.4 without assigning unknown semantics. */
[[nodiscard]] bool skip_group_0_1_4(Reader& reader) noexcept {
    for (const std::uint8_t width : kGroup_0_1_4OptionalWidths) {
        if (!skip_optional_bits(reader, width)) {
            return false;
        }
    }
    // Field 8 is required u64, field 9 is optional 3-bit, and field 10 is required bool.
    return reader.skip(kScalar64WidthBits) && skip_optional_bits(reader, kThreeWidthBits)
           && reader.skip(kBooleanWidthBits);
}

/** Consumes present fixed-width group 0.1.5. */
[[nodiscard]] bool skip_group_0_1_5(Reader& reader) noexcept {
    return reader.skip(fixed_array_width_bits(kGroup_0_1_5ValueCount, kScalar32WidthBits));
}

/** Consumes present nested entry array 0.1.6.0. */
[[nodiscard]] bool skip_group_0_1_6_0(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kGroup_0_1_6EntryCount; ++index) {
        if (!skip_optional_bits(reader, kScalar16WidthBits)) {
            return false;
        }
    }
    return reader.skip(fixed_array_width_bits(kGroup_0_1_6FixedWordCount, kScalar32WidthBits));
}

/** Consumes present group 0.1.6, including optional children 0 and 1. */
[[nodiscard]] bool skip_group_0_1_6(Reader& reader) noexcept {
    return read_optional_group(reader, skip_group_0_1_6_0)
           && skip_optional_bits(reader, kScalar16WidthBits);
}

/** Consumes present fixed-tail group 0.1.12. */
[[nodiscard]] bool skip_group_0_1_12(Reader& reader) noexcept {
    return reader.skip(fixed_array_width_bits(kGroup_0_1_12ValueCount, kScalar16WidthBits))
           && reader.skip(fixed_array_width_bits(kGroup_0_1_12FixedWordCount, kScalar32WidthBits));
}

/** Traverses every child of present account branch 0.1 in descriptor order. */
[[nodiscard]] bool read_account_branch(Reader& reader, settings::SettingsDelta& delta) noexcept {
    // 0.1.0: calibration vectors.
    if (!read_optional_group(reader, skip_group_0_1_0)) {
        return false;
    }

    // 0.1.1: preference scalars and the optional 3-by-50 matrix.
    if (!read_optional_group(reader, [&delta](Reader& groupReader) noexcept {
            return read_preference_record(groupReader, delta);
        })) {
        return false;
    }

    // 0.1.2: local mirrors, keybinding source, and packed binding table.
    if (!read_optional_group(reader, [&delta](Reader& groupReader) noexcept {
            return read_binding_record(groupReader, delta);
        })) {
        return false;
    }

    // 0.1.3: four required 16-bit values.
    if (!read_optional_group(reader, skip_group_0_1_3)) {
        return false;
    }

    // 0.1.4: mixed-width record with unknown semantics.
    if (!read_optional_group(reader, skip_group_0_1_4)) {
        return false;
    }

    // 0.1.5: 22 required 32-bit values.
    if (!read_optional_group(reader, skip_group_0_1_5)) {
        return false;
    }

    // 0.1.6: optional 100-entry region, two required words, and optional 16-bit tail.
    if (!read_optional_group(reader, skip_group_0_1_6)) {
        return false;
    }

    // 0.1.7: optional Boolean.
    if (!skip_optional_bits(reader, kBooleanWidthBits)) {
        return false;
    }

    // 0.1.8: optional Boolean.
    if (!skip_optional_bits(reader, kBooleanWidthBits)) {
        return false;
    }

    // 0.1.9: optional Boolean.
    if (!skip_optional_bits(reader, kBooleanWidthBits)) {
        return false;
    }

    // 0.1.10: optional 8-bit scalar.
    if (!skip_optional_bits(reader, kByteWidthBits)) {
        return false;
    }

    // 0.1.11: optional 32-bit scalar.
    if (!skip_optional_bits(reader, kScalar32WidthBits)) {
        return false;
    }

    // 0.1.12: 30 required 16-bit values followed by two required 32-bit words.
    if (!read_optional_group(reader, skip_group_0_1_12)) {
        return false;
    }

    // 0.1.13: optional 32-bit scalar.
    return skip_optional_bits(reader, kScalar32WidthBits);
}

/** Consumes both optional length-prefixed blobs following the reflected object. */
[[nodiscard]] bool skip_outer_blobs(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kOuterBlobCount; ++index) {
        bool present = false;
        std::uint64_t byteCount = 0;
        if (!read_presence(reader, present)) {
            return false;
        }
        if (present
            && (!reader.read(kOuterBlobLengthWidthBits, byteCount)
                || !reader.skip(static_cast<std::size_t>(byteCount) * kByteWidthBits))) {
            return false;
        }
    }
    return true;
}

/** Requires any final partial byte to contain only zero padding. */
[[nodiscard]] bool finish_padding(Reader& reader) noexcept {
    const std::size_t remaining = reader.remaining_bits();
    if (remaining >= kTerminalPaddingLimitBits) {
        return false;
    }
    std::uint64_t padding = 0;
    return reader.read(static_cast<std::uint8_t>(remaining), padding) && padding == 0
           && reader.remaining_bits() == 0;
}

/** Compile-time proof that the declared traversal still matches the catalog's maximum form. */
namespace schema_size_proof {

[[nodiscard]] consteval std::size_t optional_scalar(std::size_t widthBits) noexcept {
    return kPresenceWidthBits + widthBits;
}

[[nodiscard]] consteval std::size_t optional_group(std::size_t bodyBits) noexcept {
    return kPresenceWidthBits + bodyBits;
}

[[nodiscard]] consteval std::size_t optional_scalar_array(std::size_t count,
                                                          std::size_t widthBits) noexcept {
    return count * optional_scalar(widthBits);
}

[[nodiscard]] consteval std::size_t preference_fields() noexcept {
    std::size_t total = 0;
    for (const PreferenceDescriptor& descriptor : kPreferenceDescriptors) {
        total += optional_scalar(descriptor.encoding.wireWidthBits);
    }
    return total;
}

[[nodiscard]] consteval std::size_t group_0_1_4_body() noexcept {
    std::size_t total = 0;
    for (const std::uint8_t widthBits : kGroup_0_1_4OptionalWidths) {
        total += optional_scalar(widthBits);
    }
    // Field 8 is required u64, field 9 is optional 3-bit, and field 10 is required bool.
    return total + kScalar64WidthBits + optional_scalar(kThreeWidthBits) + kBooleanWidthBits;
}

constexpr std::size_t kPublicityExpiryBankBits =
    optional_group(optional_scalar_array(kPublicityExpiryCount, kScalar64WidthBits));
constexpr std::size_t kSeenMessageBankBits =
    optional_group(fixed_array_width_bits(kSeenMessageCount, kScalar32WidthBits));
constexpr std::size_t kRootMetadataBits =
    optional_group(kPublicityExpiryBankBits + kSeenMessageBankBits);

constexpr std::size_t kCalibrationGroupBits = optional_group(
    kCalibrationVectorCount
    * optional_group(fixed_array_width_bits(kCalibrationValuesPerVector, kScalar32WidthBits)));
constexpr std::size_t kPreferenceMatrixBits = optional_group(optional_scalar_array(
    kPreferenceMatrixRowCount * kPreferenceMatrixColumnCount, kScalar32WidthBits));
constexpr std::size_t kPreferencesBits =
    optional_group(preference_fields() + kPreferenceMatrixBits);
constexpr std::size_t kBindingsBits =
    optional_group(optional_scalar(kScalar32WidthBits) + optional_scalar(kBooleanWidthBits)
                   + optional_scalar(kThreeWidthBits) + optional_scalar(kScalar32WidthBits)
                   + optional_scalar(kBooleanWidthBits)
                   + optional_group(fixed_array_width_bits(kKeyBindingCount, kScalar32WidthBits)));
constexpr std::size_t kGroup_0_1_3Bits =
    optional_group(fixed_array_width_bits(kGroup_0_1_3ValueCount, kScalar16WidthBits));
constexpr std::size_t kGroup_0_1_4Bits = optional_group(group_0_1_4_body());
constexpr std::size_t kGroup_0_1_5Bits =
    optional_group(fixed_array_width_bits(kGroup_0_1_5ValueCount, kScalar32WidthBits));
constexpr std::size_t kGroup_0_1_6Bits = optional_group(
    optional_group(optional_scalar_array(kGroup_0_1_6EntryCount, kScalar16WidthBits)
                   + fixed_array_width_bits(kGroup_0_1_6FixedWordCount, kScalar32WidthBits))
    + optional_scalar(kScalar16WidthBits));
constexpr std::size_t kAccountTailBits =
    optional_scalar(kBooleanWidthBits) + optional_scalar(kBooleanWidthBits)
    + optional_scalar(kBooleanWidthBits) + optional_scalar(kByteWidthBits)
    + optional_scalar(kScalar32WidthBits);
constexpr std::size_t kGroup_0_1_12Bits =
    optional_group(fixed_array_width_bits(kGroup_0_1_12ValueCount, kScalar16WidthBits)
                   + fixed_array_width_bits(kGroup_0_1_12FixedWordCount, kScalar32WidthBits));
constexpr std::size_t kGroup_0_1_13Bits = optional_scalar(kScalar32WidthBits);

constexpr std::size_t kAccountBranchBits =
    optional_group(kCalibrationGroupBits + kPreferencesBits + kBindingsBits + kGroup_0_1_3Bits
                   + kGroup_0_1_4Bits + kGroup_0_1_5Bits + kGroup_0_1_6Bits + kAccountTailBits
                   + kGroup_0_1_12Bits + kGroup_0_1_13Bits);
constexpr std::size_t kSchemaBits = kRootMetadataBits + kAccountBranchBits;

static_assert(kRootMetadataBits == kCatalogRootMetadataMaximumBits);
static_assert(kPreferencesBits == kCatalogPreferencesMaximumBits);
static_assert(kBindingsBits == kCatalogBindingsMaximumBits);
static_assert(kAccountBranchBits == kCatalogAccountBranchMaximumBits);
static_assert(kSchemaBits == kCatalogMaximumBits);

} // namespace schema_size_proof

} // namespace

/** Decodes the complete schema-0x80807603 request without touching authoritative State. */
bool parse_request(const Message& message, Request& output) noexcept {
    output = {};
    if (message.opcode != kOpcode) {
        return false;
    }

    Reader reader(message.payload);
    Request candidate{};
    if (!read_optional_group(reader, skip_publicity_and_seen_messages)
        || !read_optional_group(reader,
                                [&candidate](Reader& groupReader) noexcept {
                                    return read_account_branch(groupReader, candidate.settings);
                                })
        || !skip_outer_blobs(reader) || !finish_padding(reader)) {
        return false;
    }

    output = candidate;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode701
