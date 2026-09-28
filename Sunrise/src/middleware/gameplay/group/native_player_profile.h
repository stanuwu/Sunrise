#pragma once

#include <array>

#include "player_block.h"

namespace sunrise::middleware::gameplay::group {
/** Up to 64 code units; shorter names include a terminator within this capacity. */
inline constexpr std::size_t kNativePlayerNameCapacity = 64;
/** Bytes of the native player-identity blob. */
inline constexpr std::size_t kNativePlayerIdentitySize = 36;
/**
 * Presence bits of the native B mask, one per optional field.
 * The mask also reserves `0x1` for the name, `0x2` for the identity and `0x20` for the soids;
 * those three carry their own presence flags in the struct below instead.
 */
namespace field_bit {
/** Presence bit for `q3`. */
inline constexpr std::uint16_t kQ3 = 0x004;
/** Presence bit for `q4`. */
inline constexpr std::uint16_t kQ4 = 0x008;
/** Presence bit for `q6`. */
inline constexpr std::uint16_t kQ6 = 0x010;
/** Presence bit for `q8Handle`, `q8Slots` and `q8Tail` together. */
inline constexpr std::uint16_t kQ8 = 0x040;
/** Presence bit for `q9`. */
inline constexpr std::uint16_t kQ9 = 0x080;
/** Presence bit for `q5`. */
inline constexpr std::uint16_t kQ5 = 0x100;
} // namespace field_bit
/** Bit of `tailFlags` that says the optional tail index follows it. */
inline constexpr std::uint8_t kTailIndexPresent = 0x10;
/** The wire carries the Q8 slots unsigned; this bias returns them to the schema's signed form. */
inline constexpr std::uint16_t kQ8SlotBias = 0x8000;
/** Fields published by this player through native player-add/player-properties messages. */
struct NativePlayerProfile final {
    std::array<char16_t, kNativePlayerNameCapacity> name{};
    std::array<std::byte, kNativePlayerIdentitySize> identity{};
    PlayerBlockSoids soids{};
    std::uint8_t nameLength{};
    bool hasName{};
    bool hasIdentity{};
    /** Native B mask for Q3/Q4/Q5/Q6/Q8/Q9; name, identity and soids have their own presence. */
    std::uint16_t fields{};
    std::uint8_t q3{}, q4{}, q5{};
    std::array<std::int16_t, 2> q6{};
    std::uint32_t q8Handle{};
    std::array<std::uint16_t, 4> q8Slots{};
    std::uint8_t q8Tail{};
    std::uint32_t q9{};
    std::array<std::uint32_t, 3> tailWords{};
    std::uint8_t tailFlags{}, tailKind{};
    bool tailFlag{}, hasTail{};
    std::uint32_t tailIndex{};
};
/** Reads all nine optional B fields, including the content-schema signed-short Q8 slots. */
[[nodiscard]] bool read_native_player_profile(encoding::bits::Reader& reader,
                                              NativePlayerProfile& output) noexcept;
/** Writes exactly the retained B fields; never fills an absent field with a default. */
[[nodiscard]] bool write_native_player_profile(encoding::bits::Writer& writer,
                                               const NativePlayerProfile& profile) noexcept;
/** @return True when every optional field, and the tail if present, fits its wire range. */
[[nodiscard]] bool valid_native_player_profile(const NativePlayerProfile& profile) noexcept;
/** @return True when `profile` carries every optional B field, the name, identity and tail. */
[[nodiscard]] bool complete_native_player_profile(const NativePlayerProfile& profile) noexcept;
/** @return False on a short read; `profile` is left unchanged until the whole tail is read. */
[[nodiscard]] bool read_native_player_tail(encoding::bits::Reader& reader,
                                           NativePlayerProfile& profile) noexcept;
/** @return False when `profile.hasTail` is unset or the profile itself fails validation. */
[[nodiscard]] bool write_native_player_tail(encoding::bits::Writer& writer,
                                            const NativePlayerProfile& profile) noexcept;
/**
 * Merges an incoming update into `target`, field by field.
 * @param update Fields it carries present replace `target`'s; absent fields leave it unchanged.
 */
void merge_native_player_profile(NativePlayerProfile& target,
                                 const NativePlayerProfile& update) noexcept;
/**
 * Native decoded B image, used both by the membership mirror and the native baseline checksum.
 * Its extent is the one the checksum hashes, so it may not be trimmed to the fields written.
 */
using NativePlayerProfileState = std::array<std::byte, 0xE8>;
/**
 * Lays `profile` out as the native decoder would, field by field.
 * The name units go in obfuscated, because that is the form the native record holds: the wire
 * carries them plain and the native reader applies this same transform on the way in.
 */
void build_native_player_profile_state(const NativePlayerProfile& profile,
                                       NativePlayerProfileState& output) noexcept;
} // namespace sunrise::middleware::gameplay::group
