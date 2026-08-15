#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::middleware::bap::activity_host_manager::request::selection {

/** The descriptor schema holds 16 skull selection entries. */
inline constexpr std::size_t kActivityManagerSkullCapacity = 16;
/** The descriptor has one fixed 40-byte signed package-name field. */
inline constexpr std::size_t kActivityManagerPackageNameCapacity = 40;
/**
 * Storage for one descriptor's exact bits, which another message replays as-is. With every
 * dynamic count at its largest the descriptor is 1,814 bits, and the common form is 372, so this
 * holds the biggest encoding the schema can make.
 */
inline constexpr std::size_t kActivityManagerDescriptorCapacity = 232;

/** Two biased scalar values from one skull-selection entry. */
struct ActivityManagerSkullValue final {
    /** 2-bit value with the required bias of 1 removed. */
    std::int8_t group{};
    /** 7-bit value with the required bias of 1 removed. */
    std::int8_t value{};
};

/** Safe scalar fields decoded from one service-6 field-one activity selection. */
struct ActivityManagerSelection final {
    /** 2-bit root request kind, required bias of 1 removed. */
    std::int8_t requestKind{};
    /** 4-bit selection reason, required bias of 1 removed. */
    std::int8_t reason{};
    /** 12-bit source activity index, required bias of 1 removed. */
    std::int16_t sourceActivityIndex{};
    /** 12-bit destination activity index, required bias of 1 removed. */
    std::int16_t activityIndex{};
    /** True when the optional 9-bit element index was present. */
    bool hasElementIndex{};
    /** Element index, required bias of 1 removed. */
    std::int16_t elementIndex{};
    /** Number of filled entries in skulls. */
    std::uint8_t skullCount{};
    /** Gameplay selections. Entries past skullCount stay zero. */
    std::array<ActivityManagerSkullValue, kActivityManagerSkullCapacity> skulls{};
    /** True when the optional arrival-bubble hash was present. */
    bool hasArrivalBubbleHash{};
    /** Unsigned arrival-bubble hash from the descriptor. */
    std::uint32_t arrivalBubbleHash{};
    /** True when the optional spawn-set hash was present. */
    bool hasSpawnSetHash{};
    /** Unsigned spawn-set hash from the descriptor. */
    std::uint32_t spawnSetHash{};
    /** True when the fixed package-name field was present. */
    bool hasPackageName{};
    /** Length of the valid lowercase prefix, 1 to 40. */
    std::uint8_t packageNameLength{};
    /** Checked package-name bytes, then zero padding. */
    std::array<std::int8_t, kActivityManagerPackageNameCapacity> packageName{};
    /** First name byte's bit offset from the descriptor's first bit. Valid with hasPackageName. */
    std::size_t packageNameBitOffset{};
    /** Last root boolean. Its gameplay meaning is not known. */
    bool trailingFlag{};
    /**
     * The descriptor's own bits, left-aligned, exactly as the client encoded them. The host echoes
     * this descriptor back in another message, and it carries fields with no known name.
     * Re-encoding from the scalars above would drop those, so the bits are replayed instead.
     */
    std::array<std::byte, kActivityManagerDescriptorCapacity> descriptorBits{};
    /** Meaningful bits in descriptorBits. The last byte may hold padding in its low bits. */
    std::size_t descriptorBitLength{};
};

/**
 * Optional typed selections from a service-6 protobuf with valid structure. The request carries
 * the same descriptor twice, in protobuf field one and inside field two, and either copy may be
 * missing or unreadable while the other is fine.
 */
struct ActivityManagerSelectionResult final {
    /** False when protobuf field one is missing, empty or unreadable. */
    bool hasSelection{};
    /** Field-one output. Valid only when hasSelection is true. */
    ActivityManagerSelection selection{};
    /** False when field two is missing or its descriptor did not read back. */
    bool hasSecondary{};
    /** Field-two output. Valid only when hasSecondary is true. */
    ActivityManagerSelection secondary{};
};

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
