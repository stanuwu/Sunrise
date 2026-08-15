#pragma once

#include <cstdint>
#include <limits>

namespace sunrise::state::activity::membership {

/** Zero means no published or acknowledged membership revision. */
inline constexpr std::uint32_t kAbsentRevision = 0;
/** The first full identity snapshot starts at revision 1. */
inline constexpr std::uint32_t kInitialRevision = 1;
/** Membership revisions stop before wrap, so a stale acknowledgement cannot look current. */
inline constexpr std::uint32_t kMaximumMembershipRevision =
    (std::numeric_limits<std::uint32_t>::max)();
/** A steady zero epoch keeps the client peer table across unchanged refreshes. */
inline constexpr std::uint32_t kStableEpoch = 0;
/** The first local transition uses token 1. */
inline constexpr std::uint8_t kInitialTransitionToken = 1;
/** The mirrored 3-bit state field decodes down to -1. */
inline constexpr std::int8_t kMinimumMirroredState = -1;
/** The mirrored 3-bit state field reaches logical 6. */
inline constexpr std::int8_t kMaximumMirroredState = 6;
/** -1 means no teleport slice set is chosen. */
inline constexpr std::int32_t kAbsentSliceSetIndex = -1;
/** The mirrored 10-bit slice-set field reaches logical 1022. */
inline constexpr std::int32_t kMaximumSliceSetIndex = 1022;
/** Bubble -1 means no refresh bubble is chosen. */
inline constexpr std::int32_t kMinimumRefreshBubble = -1;
/** Membership refreshes reach at most the 64 bubble slots. */
inline constexpr std::int32_t kMaximumRefreshBubble = 63;
/** All-one bits are not a usable secondary member SOID. */
inline constexpr std::uint64_t kInvalidOpaqueSoid = (std::numeric_limits<std::uint64_t>::max)();
/** All-one bits are not a usable per-join lookup identity. */
inline constexpr std::uint64_t kInvalidJoinIdentity = (std::numeric_limits<std::uint64_t>::max)();

/** Client identity, kept without its source payload bytes. */
struct Identity final {
    std::uint64_t memberKey{};
    std::int32_t smallOpaque{};
    std::int32_t signedOpaque{};
    std::uint64_t joinIdentity{};
    std::uint64_t accountSoid{};
    /** Its role is not verified. Only the nonzero SOID rule is known. */
    std::uint64_t opaqueSoid{};
    /** The role of this second type-23 scalar is not verified. */
    std::uint64_t secondaryOpaque{};
};

/** Spawn state kept for the current activity host, in no wire form. */
struct SpawnState final {
    std::int8_t state{};
    std::uint8_t opaqueByte{};
    std::uint64_t opaqueValue{};
};

/** Teleport state kept for the current activity host, in no wire form. */
struct TeleportState final {
    std::int8_t state{};
    std::uint8_t token{};
    std::int32_t sliceSetIndex{kAbsentSliceSetIndex};
    std::uint32_t sliceSetHash{};
};

/** -1 means the client reported no region. */
inline constexpr std::int32_t kAbsentRegionIndex = -1;
/** The mirrored 10-bit region field reaches logical 1022. */
inline constexpr std::int32_t kMaximumRegionIndex = 1022;

/**
 * The region the client reports it is in, and that region's name hash.
 * It beats the destination's own slice set wherever the host names the player's position, and
 * it moves as the player crosses a bubble boundary.
 */
struct RegionState final {
    std::int32_t index{kAbsentRegionIndex};
    std::uint32_t hash{};
};

/** Sparse activity-host changes taken by one State transaction. */
struct AuthoritativeUpdate final {
    SpawnState spawn{};
    TeleportState teleport{};
    RegionState region{};
    std::uint8_t transitionToken{};
    bool hasTransitionToken{};
    bool hasSpawn{};
    bool hasTeleport{};
    bool hasRegion{};
};

/** Data read under one lock, enough to encode a full membership refresh. */
struct Snapshot final {
    Identity identity{};
    SpawnState spawn{};
    TeleportState teleport{};
    std::uint32_t revision{};
    std::uint32_t epoch{kStableEpoch};
    std::uint8_t transitionToken{};
};

/** Mutable membership fields owned by one activity session. */
struct MembershipState final {
    Identity identity{};
    SpawnState spawn{};
    TeleportState teleport{};
    /** Last region the client reported, kept so a repeat of the same one is not a move. */
    RegionState region{};
    std::uint32_t revision{};
    std::uint32_t acknowledgedRevision{};
    std::uint8_t transitionToken{};
    /** Tells an explicit zero token apart from the initial fallback token. */
    bool hasTransitionToken{};
    bool hasIdentity{};
};

/** What one prepared membership transaction does. */
enum class MutationKind : std::uint8_t {
    none,
    identity,
    authoritative,
    refresh,
    acknowledgement,
};

} // namespace sunrise::state::activity::membership
