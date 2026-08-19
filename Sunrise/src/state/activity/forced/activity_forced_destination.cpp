#include "activity_forced_destination.h"

#include <Windows.h>

#include <cstddef>
#include <span>

#include "../../runtime/storage/internal.h"

namespace sunrise::state::activity::forced {
namespace {

/** Bits in one byte. The name field is 40 of them, back to back. */
constexpr std::size_t kBitsPerByte = 8;
/** Every name byte is encoded with this bias, and padding is a biased zero. */
constexpr unsigned kPackageNameBias = 128;
/** The most significant bit of a byte, where each packed field starts. */
constexpr unsigned kHighBit = 0x80;
/** Reads one most-significant-bit-first value from descriptor storage. */
[[nodiscard]] constexpr bool read_bit(std::span<const std::byte> bits,
                                      std::size_t bitOffset) noexcept {
    const unsigned mask = kHighBit >> (bitOffset % kBitsPerByte);
    return (static_cast<unsigned>(bits[bitOffset / kBitsPerByte]) & mask) != 0;
}

/** Writes one most-significant-bit-first value into descriptor storage. */
constexpr void write_bit(std::span<std::byte> bits, std::size_t bitOffset, bool value) noexcept {
    std::byte& target = bits[bitOffset / kBitsPerByte];
    const unsigned mask = kHighBit >> (bitOffset % kBitsPerByte);
    target = static_cast<std::byte>(value ? static_cast<unsigned>(target) | mask
                                          : static_cast<unsigned>(target) & ~mask);
}

/**
 * Writes one byte into bit-packed storage at a bit offset.
 * @param bits Storage large enough to hold the whole byte at that offset.
 * @param bitOffset First bit of the byte.
 * @param value Byte to write, most significant bit first.
 */
void write_byte(std::span<std::byte> bits, std::size_t bitOffset, unsigned value) noexcept {
    for (std::size_t index = 0; index < kBitsPerByte; ++index) {
        const bool set = (value >> (kBitsPerByte - 1 - index) & 1U) != 0;
        write_bit(bits, bitOffset + index, set);
    }
}

/** Inserts the fixed name payload after an optional field's clear presence bit. */
[[nodiscard]] constexpr bool
insert_name_field(destination::DestinationSelection& selection) noexcept {
    const std::size_t nameBits = destination::kPackageNameCapacity * kBitsPerByte;
    const std::size_t oldLength = selection.descriptorBitLength;
    const std::size_t presenceBit = selection.descriptorNamePresenceBit;
    const std::size_t tailBit = presenceBit + 1;
    if (oldLength == 0 || presenceBit >= oldLength
        || oldLength + nameBits > selection.descriptorBits.size() * kBitsPerByte) {
        return false;
    }

    for (std::size_t sourceEnd = oldLength; sourceEnd > tailBit; --sourceEnd) {
        const std::size_t sourceBit = sourceEnd - 1;
        write_bit(selection.descriptorBits,
                  sourceBit + nameBits,
                  read_bit(selection.descriptorBits, sourceBit));
    }
    for (std::size_t bit = tailBit; bit < tailBit + nameBits; ++bit) {
        write_bit(selection.descriptorBits, bit, false);
    }
    write_bit(selection.descriptorBits, presenceBit, true);
    selection.descriptorBitLength = static_cast<std::uint16_t>(oldLength + nameBits);
    selection.descriptorNameBit = static_cast<std::uint16_t>(tailBit);
    selection.hasDescriptorName = true;
    return true;
}

/** Proves insertion leaves every opaque tail bit in order. */
[[nodiscard]] constexpr bool insertion_preserves_tail() noexcept {
    destination::DestinationSelection selection{};
    selection.descriptorBitLength = 13;
    selection.descriptorNamePresenceBit = 4;
    write_bit(selection.descriptorBits, 5, true);
    write_bit(selection.descriptorBits, 12, true);
    if (!insert_name_field(selection)) {
        return false;
    }
    return selection.descriptorBitLength == 333 && selection.descriptorNameBit == 5
           && selection.hasDescriptorName && read_bit(selection.descriptorBits, 4)
           && read_bit(selection.descriptorBits, 325) && !read_bit(selection.descriptorBits, 326)
           && read_bit(selection.descriptorBits, 332);
}

static_assert(insertion_preserves_tail());

/**
 * Rewrites the captured descriptor's package name with the forced one.
 * @param selection Committed destination holding the captured bits.
 * @param value Forced destination whose name replaces the captured one.
 * @return True when the whole 40-byte field sat inside the captured bits.
 */
[[nodiscard]] bool rename_descriptor(destination::DestinationSelection& selection,
                                     const ForcedDestination& value) noexcept {
    const std::size_t nameBits = destination::kPackageNameCapacity * kBitsPerByte;
    if ((!selection.hasDescriptorName && !insert_name_field(selection))
        || selection.descriptorBitLength == 0
        || selection.descriptorNameBit + nameBits > selection.descriptorBitLength) {
        return false;
    }
    for (std::size_t index = 0; index < destination::kPackageNameCapacity; ++index) {
        // Past the name the field is padding, and a biased zero decodes outside the name charset,
        // which is what ends the name.
        const unsigned character = index < value.packageNameLength
                                       ? static_cast<unsigned char>(value.packageName[index])
                                       : 0U;
        write_byte(selection.descriptorBits,
                   selection.descriptorNameBit + index * kBitsPerByte,
                   character + kPackageNameBias & 0xFFU);
    }
    return true;
}

} // namespace

/** Replaces the forced destination. */
bool publish(const ForcedDestination& value) noexcept {
    if (!storable(value)) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.activity.forced = value;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Copies the forced destination. */
void snapshot(ForcedDestination& value) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    value = runtime::storage::g_state.activity.forced;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

/** Drops the selection and the switch, the same as the interface's clear action. */
void clear() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.activity.forced = {};
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** @return True while the stored selection is complete and its switch is on. */
bool override_active() noexcept {
    ForcedDestination value{};
    snapshot(value);
    return active(value);
}

/** Overwrites one committed destination with the forced one. */
bool apply(destination::DestinationSelection& selection) noexcept {
    ForcedDestination value{};
    snapshot(value);
    if (!active(value)) {
        return false;
    }

    selection.packageName = {};
    for (std::size_t index = 0; index < value.packageNameLength; ++index) {
        selection.packageName[index] = static_cast<std::int8_t>(value.packageName[index]);
    }
    selection.packageNameLength = value.packageNameLength;
    // A captured descriptor owns its carrier identity. With no descriptor, no activity index can
    // be derived from a package name because several definitions can name the same package.
    const bool hasCarrierDescriptor = selection.descriptorBitLength != 0;
    if (!hasCarrierDescriptor) {
        selection.reason = destination::kMinimumReason;
        selection.previousActivityIndex = destination::kAbsentActivityIndex;
        selection.activityIndex = destination::kAbsentActivityIndex;
    }
    selection.elementIndex = destination::kAbsentElementIndex;
    selection.hasElementIndex = false;
    // The client named its arrival for the destination it picked, so both wire hashes go with it.
    selection.arrivalBubbleHash = 0;
    selection.hasArrivalBubbleHash = false;
    selection.spawnSetHash = 0;
    selection.hasSpawnSetHash = false;
    selection.arrivalBubbleOverride = value.bubble;
    selection.hasArrivalBubbleOverride = true;
    selection.sliceSetOverride = value.sliceSet;
    selection.hasSliceSetOverride = true;
    // With no set chosen the absent hash goes out, so the Client searches the loaded world itself.
    // A map-wide set is not proof that the arrival bubble holds one of its points.
    selection.spawnSetOverride = value.hasSpawnSetHash ? value.spawnSetHash : kAbsentSpawnSetHash;
    selection.hasSpawnSetOverride = true;
    // The Client authors this descriptor and the host replays it. Rebuilding it from named fields
    // drops the ones with no name, and the Client then holds its lobby on Waiting for Other
    // Players.
    if (!rename_descriptor(selection, value)) {
        // Nothing usable was captured, so the reconstructed descriptor goes out instead.
        selection.descriptorBits = {};
        selection.descriptorBitLength = 0;
        selection.descriptorNameBit = 0;
        selection.descriptorNamePresenceBit = 0;
        selection.hasDescriptorName = false;
    }
    return true;
}

} // namespace sunrise::state::activity::forced
