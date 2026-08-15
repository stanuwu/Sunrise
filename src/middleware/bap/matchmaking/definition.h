#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::matchmaking {

/** A matchmaking join descriptor is exactly 128 opaque bytes. */
inline constexpr std::size_t kJoinDescriptorSize = 128;
/** A field-7 reply with a uint64 id and a full descriptor is at most 148 bytes. */
inline constexpr std::size_t kMaximumResponseBodySize = 148;

/** Request selector carried by service-42 protobuf field two. */
enum class RequestKind : std::uint8_t {
    /** Missing, malformed, or unsupported selectors produce an empty reply. */
    none = 0,
    /** Requests a session search result set. */
    sessionSearch = 1,
    /** Publishes or updates one local session advertisement. */
    advertisementUpdate = 2,
    /** Deletion label. Its normal-advertisement reply is empty. */
    advertisementDelete = 3,
    /** Requests the current matchmaking configuration. */
    configuration = 4,
    /** Publishes or updates the latest rejoin advertisement. */
    rejoinAdvertisementUpdate = 5,
    /** Deletion label. Its rejoin-advertisement reply is empty. */
    rejoinAdvertisementDelete = 6,
    /** Finds the latest advertised session. */
    locateSession = 7,
    /** Requests the current live matchmaking statistics. */
    liveStats = 8,
};

/** Borrowed fields used by a kind-2 advertisement update. */
struct AdvertisementUpdate final {
    /** True when the chosen existing-id field used the varint wire type. */
    bool hasExistingId{};
    /** Client-supplied advertisement id. Zero asks the server to assign one. */
    std::uint64_t existingId{};
    /** Opaque stable key for one advertisement record. Absent means zero. */
    std::uint64_t variantKey{};
    /** True when the chosen descriptor field carried exactly 128 bytes. */
    bool hasDescriptor{};
    /** Descriptor bytes borrowed from the request body. */
    std::span<const std::byte> descriptor{};
};

/** Checked service-42 request fields the solo responder needs. */
struct Request final {
    /** Valid selector, or none when any read protobuf scope is malformed. */
    RequestKind kind{RequestKind::none};
    /** Kind-2 fields. All values stay neutral for other request kinds. */
    AdvertisementUpdate advertisement{};
};

/** Inputs to encode one service-43 response body. */
struct Response final {
    /** Response shape, picked by the matching request kind. */
    RequestKind kind{RequestKind::none};
    /** Nonzero id needed by kinds 2, 5, and descriptor-bearing 7. */
    std::uint64_t advertisementId{};
    /** Optional full descriptor borrowed from the process-local runtime State. */
    std::span<const std::byte> descriptor{};
};

} // namespace sunrise::middleware::bap::matchmaking
