#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::datagen::family4::instance::layout {

/** Every item instance carries 12 fixed ordinary socket lanes. */
inline constexpr std::size_t kOrdinarySocketCapacity = 12;
/** Each ordinary socket carries 2 unresolved auxiliary hash lanes. */
inline constexpr std::size_t kSocketAuxiliaryHashCount = 2;
/** The replicated roll state carries 12 3-byte selector lanes. */
inline constexpr std::size_t kSelectorCapacity = 12;
/** Socket-entry-list state reserves 36 1-byte lanes. */
inline constexpr std::size_t kSocketEntryStateCapacity = 36;
/** The native random-roll region reserves 8 opaque bytes. */
inline constexpr std::size_t kRandomRollByteCount = 8;
/** The optional creation request reserves 6 fixed entry lanes. */
inline constexpr std::size_t kCreationEntryCapacity = 6;
/** Each creation entry carries 4 biased definition-index words. */
inline constexpr std::size_t kCreationDefinitionCount = 4;
/** 8 signed lanes form the unresolved instance-record tail. */
inline constexpr std::size_t kTailValueCount = 8;
/** Every set bit is the native empty biased definition index. */
inline constexpr std::uint16_t kEmptyDefinitionIndex = 0xFFFF;
/** The definition-hash type uses the engine no-definition sentinel. */
inline constexpr std::uint32_t kNoDefinitionHash = 0x811C9DC5;
/** The signed native lookup ABI exposes 32,768 nonnegative definition indices. */
inline constexpr std::uint32_t kDefinitionIndexCapacity = 1U << 15U;
/** Every set bit leaves all 12 socket gate lanes permitted. */
inline constexpr std::uint32_t kAllSocketBits = 0xFFFFFFFF;
/** Every set bit marks one 3-byte selector lane as absent. */
inline constexpr std::uint8_t kEmptySelectorByte = 0xFF;
/** 0 leaves unresolved auxiliary socket hashes inert in the native item record. */
inline constexpr std::uint32_t kSafeSocketAuxiliaryHash = 0;
/** 0 leaves each client-derived socket mask empty before its first rebuild. */
inline constexpr std::uint32_t kInitialDerivedSocketMask = 0;
/** A newly generated instance has no acknowledged per-item progression yet. */
inline constexpr std::int32_t kInitialInstanceProgress = 0;
/** 0 is the lowest level accepted by the native signed item-level field. */
inline constexpr std::int32_t kMinimumItemLevel = 0;
/** 0 is the neutral generated level-curve selector. */
inline constexpr std::uint16_t kInitialLevelCurveX = 0;
/** Generated item instances use the first native level-cap row. */
inline constexpr std::uint16_t kInitialLevelCapRow = 0;
/** New item instances carry no optional creation-request entries. */
inline constexpr std::int32_t kEmptyCreationEntryCount = 0;
/** The native level-state block occupies 8 bytes. */
inline constexpr std::size_t kLevelStateSize = 8;
/** One native ordinary-socket lane occupies 12 bytes. */
inline constexpr std::size_t kOrdinarySocketSize = 12;
/** Ordinary sockets and their 5 masks occupy 164 bytes. */
inline constexpr std::size_t kOrdinarySocketBlockSize = 164;
/** One native socket selector occupies 3 bytes. */
inline constexpr std::size_t kSocketSelectorSize = 3;
/** The replicated socket and roll state occupies 100 bytes. */
inline constexpr std::size_t kRollStateSize = 100;
/** One native creation-request entry occupies 12 bytes. */
inline constexpr std::size_t kCreationEntrySize = 12;
/** The optional native creation request occupies 96 bytes. */
inline constexpr std::size_t kCreationRequestSize = 96;
/** The Family-4 item-instance native schema owns exactly 416 bytes. */
inline constexpr std::size_t kObjectSize = 416;

#pragma pack(push, 1)

/** Item level and the 2 native curve selectors that bound it. */
struct LevelState {
    std::int32_t level{};
    std::uint16_t curveX{};
    std::uint16_t capRow{};
};

/** One ordinary socket's plug definition and 2 unresolved native hash lanes. */
struct OrdinarySocket {
    std::uint16_t plugDefinitionIndex{};
    std::uint16_t reserved{};
    std::array<std::uint32_t, kSocketAuxiliaryHashCount> auxiliaryHashes{};
};

/**
 * 12 ordinary sockets followed by their 5 replicated state masks.
 * A plug source passes only when its set mask holds the socket bit and its clear mask does not.
 */
struct OrdinarySocketBlock {
    std::array<OrdinarySocket, kOrdinarySocketCapacity> sockets{};
    /** Set to pass a plug read from this instance's own socket lanes. */
    std::uint32_t activeMask{};
    /** Set to pass a plug read from the item definition's declared socket block. */
    std::uint32_t definitionUnlockMask{};
    /** Clear to pass a plug read from this instance's own socket lanes. */
    std::uint32_t blockedMask{};
    /** Clear to pass a plug read from the item definition's declared socket block. */
    std::uint32_t expressionUnlockMask{};
    /** The client ANDs this into `activeMask`, so a zero blanks every socket's state. */
    std::uint32_t gateMask{};
};

/** One native 3-byte selector into a socket-entry-list row. */
struct SocketSelector {
    std::uint8_t entry{};
    std::uint8_t group{};
    std::uint8_t element{};
};

/** Replicated progression, selector, roll, and socket-entry state for one item. */
struct RollState {
    std::int32_t progress{};
    std::int32_t valueA{};
    std::int32_t valueB{};
    std::uint16_t progressionDefinitionIndex{};
    std::uint16_t reserved{};
    std::array<SocketSelector, kSelectorCapacity> selectors{};
    std::uint16_t socketEntryListIndex{};
    std::array<std::uint8_t, kRandomRollByteCount> randomRoll{};
    std::uint8_t valueC{};
    std::array<std::uint8_t, kSocketEntryStateCapacity> socketEntryStates{};
    std::uint8_t reservedTail{};
};

/** One optional item-creation request entry. */
struct CreationEntry {
    std::array<std::uint16_t, kCreationDefinitionCount> definitionIndices{};
    std::int32_t value{};
};

/** Optional creation request kept empty for an already existing item instance. */
struct CreationRequest {
    std::uint16_t primaryDefinitionIndex{};
    std::uint16_t reservedA{};
    std::array<std::uint32_t, kSocketAuxiliaryHashCount> definitionHashes{};
    std::int32_t value{};
    std::uint16_t secondaryDefinitionIndex{};
    std::uint16_t reservedB{};
    std::int32_t entryCount{};
    std::array<CreationEntry, kCreationEntryCapacity> entries{};
};

/** Byte-exact Family-4 item-instance object generated from resolved State input. */
struct Object {
    std::uint64_t instanceSoid{};
    LevelState level{};
    std::uint16_t baseDefinitionIndex{};
    std::uint16_t reserved{};
    OrdinarySocketBlock ordinarySockets{};
    RollState roll{};
    CreationRequest creation{};
    std::uint16_t tailDefinitionIndex{};
    std::uint16_t tailReserved{};
    std::array<std::int32_t, kTailValueCount> tailValues{};
};

#pragma pack(pop)

static_assert(sizeof(LevelState) == kLevelStateSize);
static_assert(sizeof(OrdinarySocket) == kOrdinarySocketSize);
static_assert(sizeof(OrdinarySocketBlock) == kOrdinarySocketBlockSize);
static_assert(sizeof(SocketSelector) == kSocketSelectorSize);
static_assert(sizeof(RollState) == kRollStateSize);
static_assert(sizeof(CreationEntry) == kCreationEntrySize);
static_assert(sizeof(CreationRequest) == kCreationRequestSize);
static_assert(sizeof(Object) == kObjectSize);
static_assert(std::is_trivially_copyable_v<Object>);

} // namespace sunrise::middleware::datagen::family4::instance::layout
