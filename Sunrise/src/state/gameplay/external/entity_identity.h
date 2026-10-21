#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::state::gameplay::entity_identity {

/** Native entity and packet sequence domains. */
inline constexpr std::size_t kSlotCapacity = 8192;
inline constexpr std::size_t kSourceCapacity = 16;
/** The native external scheduler admits at most 256 records per packet. */
inline constexpr std::size_t kObservationBatchCapacity = 256;
/**
 * One packet's records plus the anchor detaches and descendant retirements they imply.
 * Derived changes have their own headroom, so a full packet cannot refuse on one detach.
 */
inline constexpr std::size_t kBatchChangeCapacity = 2 * kObservationBatchCapacity;
inline constexpr std::uint16_t kPacketModulus = 1024;
inline constexpr std::uint16_t kNoCell = 0xFFFF;

/** A source is one admitted activity, peer channel, and replication view. */
struct Source final {
    std::uint64_t activitySessionId{}, activityRevision{}, groupSessionId{};
    std::uint64_t activityClientGeneration{};
    std::uint64_t peerGeneration{}, channelGeneration{}, viewGeneration{};
    std::uint32_t address{}, localConnectionSequence{}, remoteConnectionSequence{};
    std::uint16_t port{}, localPort{};
    bool operator==(const Source&) const = default;
};

/** Slot incarnations prevent a reused network index from aliasing an earlier entity. */
struct Token final {
    std::uint16_t slot{};
    std::uint8_t incarnation{};
    bool operator==(const Token&) const = default;
};

/** A host retirement names an allocation, never an index alone. */
struct RetiredLifetime final {
    Token token{};
    std::uint8_t allocationSequence{}, allocationEpoch{};
    std::uint64_t allocationDomain{};
    bool operator==(const RetiredLifetime&) const = default;
};

/** Exact authored squad reference carried by a network-squad baseline. */
struct SquadReference final {
    std::uint32_t key{};
    std::uint16_t index{};
    std::uint8_t type{};
    bool operator==(const SquadReference&) const = default;
};

/** A decoded actor source relation is mutable and does not classify the entity. */
struct ActorSourceReference final {
    std::uint32_t key{};
    std::uint16_t index{};
    std::uint8_t type{};
    bool known{}, present{};
    bool operator==(const ActorSourceReference&) const = default;
};

/** The first two pool positions of the compiled damage component are health then shield. */
inline constexpr std::size_t kDamagePoolCount = 2;
/** Health is the first pool position of the compiled damage component. */
inline constexpr std::size_t kHealthPoolIndex = 0;
/** Shield is the second pool position of the compiled damage component. */
inline constexpr std::size_t kShieldPoolIndex = 1;
/** The compiled damage component stores each pool as a ten-bit level. */
inline constexpr unsigned kDamagePoolLevelBits = 10;
/** Largest raw level a ten-bit damage pool can carry. */
inline constexpr std::uint16_t kDamagePoolLevelMaximum = (1U << kDamagePoolLevelBits) - 1U;

/** Raw ten-bit pools from a completely decoded damage component. Missing pools stay unknown. */
struct DamagePools final {
    std::array<std::uint16_t, kDamagePoolCount> levels{};
    std::array<bool, kDamagePoolCount> present{};
    bool operator==(const DamagePools&) const = default;
};

/** Unknown semantic fields remain absent rather than becoming player or NPC classifications. */
struct Metadata final {
    std::uint32_t rsatTag{};
    SquadReference squad{};
    std::array<std::byte, 8> playerBroadcast{};
    std::uint8_t objectType{};
    bool hasRsat{}, hasSquad{}, hasPlayerBroadcast{}, hasObjectType{};
    bool operator==(const Metadata&) const = default;
};

enum class Action : std::uint8_t { create, update, remove, createAndRemove };

/** One fully accepted wire record; the transport owns its source and packet sequence. */
struct Observation final {
    std::uint64_t allocationDomain{};
    Token token{}, anchor{};
    Metadata metadata{};
    ActorSourceReference actorSource{};
    DamagePools damage{};
    std::uint64_t tick{}, packetOrdinal{};
    std::uint16_t packetRecordIndex{};
    std::uint16_t packetSequence{}, cell{kNoCell}, recordFlags{};
    std::uint8_t allocationSequence{}, allocationEpoch{}, type{};
    bool hasAllocationEpoch{};
    Action action{Action::update};
    bool hasPacketSequence{}, hasPacketOrdinal{}, anchorChanged{}, anchorPresent{}, trailingState{};
};

/** Observed identity is evidence, not authority to delete or claim an entity. */
struct Identity final {
    std::uint64_t allocationDomain{}, serialDomain{};
    Token token{}, anchor{};
    Metadata metadata{};
    ActorSourceReference actorSource{};
    DamagePools damage{};
    std::uint64_t revision{}, tick{}, packetOrdinal{};
    std::uint16_t packetRecordIndex{};
    std::uint16_t packetSequence{}, cell{kNoCell}, recordFlags{};
    std::uint8_t allocationSequence{}, allocationEpoch{}, type{};
    bool hasAllocationEpoch{};
    bool known{}, present{}, conflicted{}, hasPacketSequence{}, hasPacketOrdinal{}, anchorKnown{},
        anchorPresent{}, trailingState{};
};

enum class Result : std::uint8_t {
    created,
    updated,
    removed,
    unchanged,
    missing,
    stale,
    conflict,
    invalid,
    capacity
};

/** Caller synchronization covers both observation and lookup operations. */
class Store final {
public:
    [[nodiscard]] std::size_t retire(const Source& source,
                                     std::span<const RetiredLifetime> lifetimes) noexcept;
    [[nodiscard]] Result observe(const Source& source, const Observation& observation) noexcept;
    [[nodiscard]] Result observe_batch(const Source& source,
                                       std::span<const Observation> observations,
                                       std::span<Result> results) noexcept;
    [[nodiscard]] Result lookup(const Source& source, Token token, Identity& output) const noexcept;
    [[nodiscard]] std::size_t sources(std::uint64_t activitySessionId,
                                      std::uint64_t activityRevision,
                                      std::span<Source> output) const noexcept;
    [[nodiscard]] Result snapshot_source(const Source& source,
                                         std::vector<Identity>& output) const noexcept;
    /** Receives one slot of a source in slot order; returning false ends the walk early. */
    using SlotVisitor = bool (*)(void* context, const Identity& slot) noexcept;
    [[nodiscard]] Result
    visit_source(const Source& source, SlotVisitor visitor, void* context) const noexcept;
    /** A committed purge advances allocation serials without deleting retained identities. */
    [[nodiscard]] bool advance_epoch(const Source&,
                                     std::uint8_t expected,
                                     std::uint8_t next,
                                     std::uint64_t nextDomain) noexcept;
    void reset_group(std::uint64_t groupSessionId) noexcept;
    void reset_source(const Source& source) noexcept;
    void reset() noexcept;

private:
    struct Partition final {
        Source source{};
        std::vector<Identity> slots{};
        bool occupied{}, hasAllocationEpoch{};
        std::uint8_t allocationEpoch{};
        std::uint64_t allocationDomain{};
    };
    std::array<Partition, kSourceCapacity> partitions_{};
    std::uint64_t revision_{};
};

} // namespace sunrise::state::gameplay::entity_identity
