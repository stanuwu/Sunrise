#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "../../../state/gameplay/external/entity_identity.h"
#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "common_state.h"

namespace sunrise::middleware::gameplay::external {

/** The native packet scheduler admits at most 256 records across its external lanes. */
inline constexpr std::size_t kEntityBatchCapacity =
    state::gameplay::entity_identity::kObservationBatchCapacity;
/** The native prelude carries a one-bit auxiliary count. */
inline constexpr std::size_t kEntityAuxiliaryCapacity = 1;
/** A token slot is the low 13 bits of the 17-bit wire token. */
inline constexpr std::uint16_t kMaximumEntitySlot = 0x1FFF;
/** A token incarnation is the high four bits of the 17-bit wire token. */
inline constexpr std::uint8_t kMaximumEntityIncarnation = 0x0F;
/** An absent or non-byte entity cell uses the 16-bit reset value. */
inline constexpr std::uint16_t kNoEntityCell = 0xFFFF;
/** One callback body is capped at 16,384 bits by host policy. */
inline constexpr std::size_t kMaximumTypePayloadBits = 16'384;
/** Callback state can retain one complete bounded body plus its typed mirror header. */
inline constexpr std::size_t kTypePayloadStateCapacity = kMaximumTypePayloadBits / 8U + 16U;

/** Record flags transported by the five explicit envelope bits. */
enum EntityRecordFlag : std::uint16_t {
    entityCreate = 0x0001,
    entityUpdate = 0x0002,
    entityRemove = 0x0004,
    entityLifecycle = 0x0008,
    entityAnchor = 0x0040,
};

/** The two-bit entity type carried by a create record. */
enum class EntityType : std::uint8_t {
    sobject = 0,
    squad = 1,
    playerBroadcast = 2,
    test = 3,
    count,
};

/** Selects the type-specific body requested from a payload codec. */
enum class TypePayloadPart : std::uint8_t {
    baseline,
    update,
};

/** One 17-bit entity identity. The simulation-world id stays in stream context. */
struct EntityToken {
    std::uint16_t slot{};
    std::uint8_t incarnation{};
};

/** Fixed callback-owned semantic state for one type payload. */
struct TypePayload {
    std::array<std::byte, kTypePayloadStateCapacity> state{};
    std::uint16_t byteCount{};
    state::gameplay::entity_identity::ActorSourceReference actorSource{};
    /** Damage component 0x80804BEE pools 0 and 1, ten-bit levels; valid when damageKnown. */
    std::uint16_t damageHealth{};
    std::uint16_t damageShield{};
    bool damageKnown{};
};

/** One generic channel-2 record and its callback-owned type state. */
struct EntityRecord {
    EntityToken token{};
    EntityToken anchor{};
    TypePayload baseline{};
    TypePayload update{};
    std::uint16_t cell{kNoEntityCell};
    std::uint16_t flags{};
    std::uint8_t allocationSequence{};
    EntityType type{EntityType::sobject};
    bool anchorPresent{};
    bool trailingState{};
    /** An anchored group omits per-record tokens and follows the receiver's retained hierarchy. */
    EntityToken streamAnchor{};
    bool implicitToken{};
    bool anchorGroupStart{};
};

/** One bounded channel-2 batch. */
struct EntityBatch {
    std::uint8_t allocationEpoch{};
    bool hasAllocationEpoch{};
    std::uint64_t allocationDomain{};
    std::bitset<kEntityBatchCapacity> ignoredRecordMask{};
    std::array<EntityToken, kEntityAuxiliaryCapacity> auxiliaryTokens{};
    EntityRecord record{};
    std::array<EntityRecord, kEntityBatchCapacity - 1> additionalRecords{};
    std::uint16_t currentCell{kNoEntityCell};
    std::uint8_t auxiliaryCount{};
    bool recordPresent{};
    std::uint16_t additionalRecordCount{};
};

[[nodiscard]] inline std::size_t entity_record_count(const EntityBatch& batch) noexcept {
    return batch.recordPresent ? 1U + batch.additionalRecordCount : 0U;
}
[[nodiscard]] inline const EntityRecord& entity_record_at(const EntityBatch& batch,
                                                          std::size_t index) noexcept {
    return index == 0 ? batch.record : batch.additionalRecords[index - 1];
}
[[nodiscard]] inline EntityRecord& entity_record_at(EntityBatch& batch,
                                                    std::size_t index) noexcept {
    return index == 0 ? batch.record : batch.additionalRecords[index - 1];
}

/** Resolves the exact existing depth-first token order of one native anchored group. */
using ResolveAnchorGroup = bool (*)(const void* context,
                                    const EntityToken& anchor,
                                    std::span<EntityToken> output,
                                    std::size_t& count) noexcept;

/** Resolves the existing type needed by an update-only record. */
using ResolveEntityType = bool (*)(const void* context,
                                   const EntityToken& token,
                                   EntityType& output) noexcept;

/** Reads one callback-owned body with its same-record baseline when available. */
using ReadTypePayload = bool (*)(const void* context,
                                 const EntityToken& token,
                                 EntityType type,
                                 TypePayloadPart part,
                                 const TypePayload* baseline,
                                 encoding::bits::Reader& reader,
                                 TypePayload& output) noexcept;

/** Writes one callback-owned body with its same-record baseline when available. */
using WriteTypePayload = bool (*)(const void* context,
                                  const EntityToken& token,
                                  EntityType type,
                                  TypePayloadPart part,
                                  const TypePayload* baseline,
                                  const TypePayload& payload,
                                  encoding::bits::Writer& writer) noexcept;

/** Cell-aware callbacks select package position widths before decoding a type body. */
using ReadCellTypePayload = bool (*)(const void* context,
                                     const EntityToken& token,
                                     EntityType type,
                                     TypePayloadPart part,
                                     const TypePayload* baseline,
                                     std::uint16_t cell,
                                     encoding::bits::Reader& reader,
                                     TypePayload& output) noexcept;
using WriteCellTypePayload = bool (*)(const void* context,
                                      const EntityToken& token,
                                      EntityType type,
                                      TypePayloadPart part,
                                      const TypePayload* baseline,
                                      const TypePayload& payload,
                                      std::uint16_t cell,
                                      encoding::bits::Writer& writer) noexcept;

/** Bounded type-payload callbacks. Empty callbacks are the safe scriptless fallback. */
struct TypePayloadCodec {
    const void* context{};
    ResolveEntityType resolveType{};
    ReadTypePayload read{};
    WriteTypePayload write{};
    std::size_t maximumBaselineBits{};
    std::size_t maximumUpdateBits{};
    ResolveAnchorGroup resolveAnchorGroup{};
    ReadCellTypePayload readForCell{};
    WriteCellTypePayload writeForCell{};
};

/** Common state plus the fixed empty channel-0, channel-1, and channel-3 profile. */
struct ExternalEntityFrame {
    CommonState common{};
    EntityBatch entities{};
    bool commonPresent{};
};

/**
 * Reads one channel-2 batch and commits the reader and output only on success.
 * TODO: no caller yet. The four entry points below wait on the packet-outcome binding to the
 * established writer.
 */
[[nodiscard]] bool read_entity_batch(encoding::bits::Reader& reader,
                                     const TypePayloadCodec& codec,
                                     EntityBatch& output) noexcept;

/** Writes one channel-2 batch after a complete fail-closed preflight. */
[[nodiscard]] bool write_entity_batch(encoding::bits::Writer& writer,
                                      const TypePayloadCodec& codec,
                                      const EntityBatch& batch) noexcept;

/** Reads the fixed four-channel wrapper and commits no state on failure. */
[[nodiscard]] bool read_external_entity_frame(encoding::bits::Reader& reader,
                                              const TypePayloadCodec& codec,
                                              ExternalEntityFrame& output) noexcept;

/** Writes the fixed four-channel wrapper after a complete fail-closed preflight. */
[[nodiscard]] bool write_external_entity_frame(encoding::bits::Writer& writer,
                                               const TypePayloadCodec& codec,
                                               const ExternalEntityFrame& frame) noexcept;

} // namespace sunrise::middleware::gameplay::external
