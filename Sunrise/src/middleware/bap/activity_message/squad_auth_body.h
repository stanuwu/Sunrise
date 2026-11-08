#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80807e.h"

namespace sunrise::middleware::bap::activity_message::squad_auth {

namespace generated = middleware::content::packages::schema::generated;

/** ClientRef slot type for the activity-local squad sensor body. */
inline constexpr std::uint8_t kSlotType = 1;
/** Runtime schema handle for the slot-type-1 auth body. */
inline constexpr std::uint32_t kSchema = generated::C80807ec9::kClass;
/**
 * The count lane is four bits wide, but the array it indexes holds eight elements.
 * The client walker does not clamp, so a count of 9 to 15 runs past the array.
 * The bound is the array, never the lane.
 */
inline constexpr std::size_t kMinimumRequestedCountLength = 1;
inline constexpr std::size_t kMaximumRequestedCountLength = 8;
/** Fixed fields and presence bits outside the requested-count values. */
inline constexpr std::size_t kBaseBitCount = 59;
/** Each requested member count is one signed 32-bit field. */
inline constexpr std::size_t kRequestedCountBitCount = 32;
/** The name hash is always sent: an absent field reads as zero, which is a real name. */
inline constexpr std::size_t kNameHashBitCount = 32;
/**
 * Spawn references `.11` and `.12`, each one nested ClientRef after a presence bit.
 * The unset form keeps the package's authored rule; only `.12` may carry a set spawn rule.
 */
inline constexpr std::size_t kSpawnReferenceBitCount = 55;
inline constexpr std::size_t kSpawnReferenceCount = 2;
/** Exact nested field-5 payload: lane selector plus four actor-definition profile values. */
inline constexpr std::size_t kAuthoredProfileBitCount = 13;
/** A present `.10` destination ClientRef and a present `.17` 31-bit request revision. */
inline constexpr std::size_t kDestinationBitCount = 55 + 31;
/** Exact meaningful bit count for one body. This is the only copy of the sum. */
[[nodiscard]] constexpr std::size_t exact_body_bit_count(std::size_t counts,
                                                         bool destination = false) noexcept {
    return kBaseBitCount + kAuthoredProfileBitCount + kSpawnReferenceBitCount * kSpawnReferenceCount
           + kRequestedCountBitCount * counts + kNameHashBitCount
           + (destination ? kDestinationBitCount : 0);
}

/** Buffer one squad body needs at the full requested-count length. */
inline constexpr std::size_t kMaximumBitCount =
    exact_body_bit_count(kMaximumRequestedCountLength, true);
inline constexpr std::size_t kMaximumByteCount = (kMaximumBitCount + 7) / 8;
/** A retained body may carry the objective fields too, up to 1,313 bits. */
inline constexpr std::size_t kMaximumRetainedBitCount = 1'313;
inline constexpr std::size_t kMaximumRetainedByteCount = (kMaximumRetainedBitCount + 7U) / 8U;
/** Spawn generation is an unsigned logical value stored in a 31-bit field. */
inline constexpr std::uint32_t kMaximumGeneration = 0x7FFFFFFF;

/** Mode 3 skips ordinary placement and keeps the requests for a passenger delivery. */
enum class Mode : std::uint8_t {
    mode0 = 0,
    mode2 = 2,
    reserve = 3,
};
[[nodiscard]] constexpr bool valid_mode(Mode mode) noexcept {
    return mode == Mode::mode0 || mode == Mode::mode2 || mode == Mode::reserve;
}

/** Last accepted positive spawn generation for one ClientRef. */
struct GenerationGuard final {
    std::uint32_t last{};
    bool hasLast{};
};

/** The type-1 squad that member actor-spawn actions fill; the client reads it from `.10`. */
struct Destination final {
    std::uint32_t registryKey{};
    std::uint16_t slotIndex{};
};

/** A type-66 spawn rule of the same object; the client uses it instead of the package rule. */
struct SpawnRule final {
    std::uint32_t registryKey{};
    std::uint16_t slotIndex{};
};

/** One canonical activity-local squad request. Zero counts select native actor destruction. */
struct Preset final {
    std::span<const std::int32_t> requestedCounts{};
    std::uint32_t generation{};
    Mode mode{Mode::mode0};
    std::optional<std::uint32_t> nameHash{};
    /** Actor-definition bytes +56..+59; field 5.0 remains the exact candidate lane zero. */
    std::array<std::int8_t, 4> authoredProfile{};
    /** Sent as `.10` with `.17` equal to the generation; absent leaves both fields out. */
    std::optional<Destination> destination{};
    /** Sent as `.12`, the rule reference the point resolver reads at +0xA0; absent keeps the
     * package rule. */
    std::optional<SpawnRule> spawnRule{};
};

/** Finds the next positive 31-bit spawn generation without wrapping. */
[[nodiscard]] bool next_generation(const GenerationGuard& guard, std::uint32_t& next) noexcept;

/**
 * Encodes one canonical slot-type-1 body without changing a refused output buffer.
 * Counts must be nonnegative and the generation must be newer than the guard.
 */
[[nodiscard]] bool encode(const Preset& preset,
                          const GenerationGuard& guard,
                          std::span<std::byte> output,
                          std::size_t& writtenBytes,
                          std::size_t& writtenBits) noexcept;

} // namespace sunrise::middleware::bap::activity_message::squad_auth
