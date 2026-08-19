#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "common_state.h"

namespace sunrise::middleware::gameplay::external {

/** The entity batch carries zero or one record. */
inline constexpr std::size_t kEntityBatchCapacity = 1;
/** A token slot is the low 13 bits of the 17-bit wire token. */
inline constexpr std::uint16_t kMaximumEntitySlot = 0x1FFF;
/** A token incarnation is the high four bits of the 17-bit wire token. */
inline constexpr std::uint8_t kMaximumEntityIncarnation = 0x0F;
/** An absent or non-byte raw bubble uses the 16-bit reset value. */
inline constexpr std::uint16_t kNoRawBubble = 0xFFFF;
/** Callback state is bounded to 256 bytes per baseline or update. */
inline constexpr std::size_t kTypePayloadStateCapacity = 256;
/** One callback body is capped at 16,384 bits by host policy. */
inline constexpr std::size_t kMaximumTypePayloadBits = 16'384;

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
};

/** One generic channel-2 record and its callback-owned type state. */
struct EntityRecord {
    EntityToken token{};
    EntityToken anchor{};
    TypePayload baseline{};
    TypePayload update{};
    std::uint16_t rawBubble{kNoRawBubble};
    std::uint16_t flags{};
    std::uint8_t lifecycleRevision{};
    EntityType type{EntityType::sobject};
    bool anchorPresent{};
    bool trailingState{};
};

/** One bounded channel-2 batch. */
struct EntityBatch {
    EntityRecord record{};
    std::uint16_t defaultRawBubble{kNoRawBubble};
    bool recordPresent{};
};

/** Resolves the existing type needed by an update-only record. */
using ResolveEntityType = bool (*)(const void* context,
                                   const EntityToken& token,
                                   EntityType& output) noexcept;

/** Reads one callback-owned baseline or update body. */
using ReadTypePayload = bool (*)(const void* context,
                                 const EntityToken& token,
                                 EntityType type,
                                 TypePayloadPart part,
                                 encoding::bits::Reader& reader,
                                 TypePayload& output) noexcept;

/** Writes one callback-owned baseline or update body. */
using WriteTypePayload = bool (*)(const void* context,
                                  const EntityToken& token,
                                  EntityType type,
                                  TypePayloadPart part,
                                  const TypePayload& payload,
                                  encoding::bits::Writer& writer) noexcept;

/** Bounded type-payload callbacks. Empty callbacks are the safe scriptless fallback. */
struct TypePayloadCodec {
    const void* context{};
    ResolveEntityType resolveType{};
    ReadTypePayload read{};
    WriteTypePayload write{};
    std::size_t maximumBaselineBits{};
    std::size_t maximumUpdateBits{};
};

/** Common state plus the fixed empty channel-0, channel-1, and channel-3 profile. */
struct ExternalEntityFrame {
    CommonState common{};
    EntityBatch entities{};
    bool commonPresent{};
};

/**
 * Reads one channel-2 batch and commits the reader and output only on success.
 * TODO: no caller yet. The four entry points below wait on the `gameplay_external_body` gate.
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
