#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::state::matchmaking {

/** 4 slots cover every local backend channel open at once, with no heap storage. */
inline constexpr std::size_t kContextCapacity = 4;
/**
 * Sunrise keeps 16 variants as headroom over the current valid ones.
 * This is not a protocol limit. It makes eviction fixed and heap-free.
 */
inline constexpr std::size_t kVariantCapacity = 16;
/** Matchmaking descriptors hold exactly 128 secret-bearing runtime bytes. */
inline constexpr std::size_t kDescriptorSize = 128;
/** A slot value outside the variant table means there is no latest variant. */
inline constexpr std::uint8_t kInvalidVariantSlot = static_cast<std::uint8_t>(kVariantCapacity);
/** Zero means an absent advertisement id on the wire. */
inline constexpr std::uint64_t kAbsentAdvertisementId = 0;
/** Advertisement ids start at 1, because 0 means absent on the wire. */
inline constexpr std::uint64_t kFirstAdvertisementId = 1;
/** A cleared generation is never a valid acquired context generation. */
inline constexpr std::uint32_t kInvalidGeneration = 0;
/** A cleared revision cannot validate a prepared State mutation. */
inline constexpr std::uint64_t kInvalidRevision = 0;
/** Context revisions start at 1, so cleared storage cannot validate a mutation. */
inline constexpr std::uint64_t kInitialContextRevision = 1;
/** Allocator revisions start at 1, so cleared storage rejects pending allocations. */
inline constexpr std::uint64_t kInitialAllocatorRevision = 1;

/** Identifies one acquired context and rejects handles from earlier acquisitions. */
struct ContextHandle {
    std::size_t slot{};
    std::uint32_t generation{};
};

/** What a prepared mutation does. */
enum class MutationKind : std::uint8_t {
    none,
    updateVariant,
    createStandaloneLatest,
};

/**
 * Fixed data needed to check and commit one prepared mutation.
 * The caller still owns the descriptor storage until commit returns.
 */
struct PendingMutation {
    MutationKind kind{};
    ContextHandle context{};
    std::uint64_t expectedContextRevision{};
    std::uint64_t expectedAllocatorRevision{};
    std::uint64_t expectedNextAdvertisementId{};
    std::uint64_t requestId{};
    std::uint64_t advertisementId{};
    std::uint64_t variant{};
    std::uint8_t targetSlot{kInvalidVariantSlot};
    bool consumesAllocator{};
    bool targetsExistingVariant{};
    bool hasDescriptor{};
    const std::byte* descriptorData{};
    std::size_t descriptorSize{};
};

/** Short-lived latest advertisement. Callers securely erase it after encoding. */
struct LatestSnapshot {
    std::uint64_t advertisementId{};
    bool hasDescriptor{};
    std::array<std::byte, kDescriptorSize> descriptor{};
};

/** One State-owned advertisement variant and its optional stored descriptor. */
struct VariantRecord {
    std::uint64_t advertisementId;
    std::uint64_t variant;
    std::uint64_t lastUpdatedRevision;
    bool occupied;
    bool hasDescriptor;
    std::array<std::byte, kDescriptorSize> descriptor;
};

/** Mutable contents securely erased whenever a logical context is released. */
struct ContextData {
    std::array<VariantRecord, kVariantCapacity> variants;
    std::uint64_t revision;
    /** Initial kind-5 allocation used only until a variant becomes latest. */
    std::uint64_t standaloneLatestId;
    /** Slot of the latest kept variant, or kInvalidVariantSlot. */
    std::uint8_t latestSlot;
};

/** Fixed context slot with a generation kept across secure data erasure. */
struct ContextSlot {
    ContextData data;
    std::uint32_t generation;
    bool active;
};

/** Context slots are plain fixed storage, fit for complete secure erasure. */
static_assert(std::is_trivial_v<ContextSlot> && std::is_standard_layout_v<ContextSlot>);

/** Process-global matchmaking state owned by the root State object. */
struct MatchmakingState {
    std::array<ContextSlot, kContextCapacity> contexts{};
    /** Next process-global nonzero id, advanced only during commit. */
    std::uint64_t nextAdvertisementId{kFirstAdvertisementId};
    /** Revision that orders the transactions using the global allocator. */
    std::uint64_t allocatorRevision{kInitialAllocatorRevision};
    /** Permanent stop that keeps ids from wrapping around. */
    bool allocatorExhausted{};
};

} // namespace sunrise::state::matchmaking
