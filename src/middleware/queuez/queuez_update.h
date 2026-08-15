#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::queuez {

/** Object payload codec stored in the family-update object header. */
enum class Encoding : std::uint32_t {
    tagReflection = 1,
    binaryDiff = 2,
    raw = 3,
    oodle = 4,
};

/** One object create, update, or delete operation. */
struct Object {
    std::uint32_t id{};
    std::uint64_t version{};
    Encoding encoding{};
    std::span<const std::byte> payload{};
};

/** One versioned family snapshot or incremental update. */
struct Family {
    std::uint32_t type{};
    std::uint64_t rootSoid{};
    std::int32_t version{};
    std::uint8_t flags{};
    std::span<const Object> objects{};
};

/** Full-snapshot flag required by a family's first update. */
inline constexpr std::uint8_t kFullSnapshotFlag = 1;

/**
 * Encodes one complete byte-aligned family-update notification body.
 * @param families Nonempty set of family updates in delivery order.
 * @param output Caller-owned notification-body storage.
 * @param written Receives the complete encoded body size.
 * @return True when all limits, raw version prefixes, and output bounds are valid.
 */
[[nodiscard]] bool encode_update(std::span<const Family> families,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

} // namespace sunrise::middleware::queuez
