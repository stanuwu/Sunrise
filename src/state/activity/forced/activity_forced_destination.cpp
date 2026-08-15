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

/**
 * Writes one byte into bit-packed storage at a bit offset.
 * @param bits Storage large enough to hold the whole byte at that offset.
 * @param bitOffset First bit of the byte.
 * @param value Byte to write, most significant bit first.
 */
void write_byte(std::span<std::byte> bits, std::size_t bitOffset, unsigned value) noexcept {
    for (std::size_t index = 0; index < kBitsPerByte; ++index) {
        const std::size_t bit = bitOffset + index;
        std::byte& target = bits[bit / kBitsPerByte];
        const unsigned mask = kHighBit >> (bit % kBitsPerByte);
        const bool set = (value >> (kBitsPerByte - 1 - index) & 1U) != 0;
        target = static_cast<std::byte>(set ? static_cast<unsigned>(target) | mask
                                            : static_cast<unsigned>(target) & ~mask);
    }
}

/**
 * Rewrites the captured descriptor's package name with the forced one.
 * @param selection Committed destination holding the captured bits.
 * @param value Forced destination whose name replaces the captured one.
 * @return True when the whole 40-byte field sat inside the captured bits.
 */
[[nodiscard]] bool rename_descriptor(destination::DestinationSelection& selection,
                                     const ForcedDestination& value) noexcept {
    const std::size_t nameBits = destination::kPackageNameCapacity * kBitsPerByte;
    if (!selection.hasDescriptorName || selection.descriptorBitLength == 0
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
    // All three are absent when a destination is forced rather than picked. Many package names
    // map to several definitions, so no index can be derived from a name.
    selection.reason = destination::kMinimumReason;
    selection.previousActivityIndex = destination::kAbsentActivityIndex;
    selection.activityIndex = destination::kAbsentActivityIndex;
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
        selection.hasDescriptorName = false;
    }
    return true;
}

} // namespace sunrise::state::activity::forced
