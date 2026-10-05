#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/gameplay/external/entity_identity.h"
#include "actor_command_runtime_codec.h"
#include "external_entity_codec.h"
#include "sobject_payload_codec.h"

namespace sunrise::middleware::gameplay::external {

using PositionProfile = middleware::bap::activity_message::wire_schema::PositionProfile;
/** Cell widths come from the admitted activity's package bounds, never the client runtime. */
using ResolvePositionProfile = bool (*)(const void* context,
                                        const state::gameplay::entity_identity::Source& source,
                                        std::uint16_t cell,
                                        PositionProfile& output) noexcept;

/** One native-compiled field group and its bitmap guard within a component. */
struct SobjectDecodeEntry final {
    std::uint32_t schemaHandle{}, repeatCount{}, guardBit{}, firstFieldBit{}, fieldBitStride{};
};
struct SobjectDecodePlan final {
    std::span<const SobjectDecodeEntry> entries{};
    std::uint32_t bitmapBits{};
    bool active{};
};
using ResolveSobjectPlan = bool (*)(const void*,
                                    std::uint32_t componentTag,
                                    std::uint32_t schemaTag,
                                    SobjectDecodePlan&) noexcept;
using ResolveSobjectSchemaLayout = bool (*)(const void*,
                                            std::uint32_t schemaHandle,
                                            std::uint32_t& serializedSize) noexcept;
using ResolveSobjectFieldLayout = bool (*)(const void*,
                                           std::uint32_t schemaHandle,
                                           std::uint32_t fieldOrdinal,
                                           std::uint32_t& bitmapOffset) noexcept;
using ResolveAdditionalSchema =
    bool (*)(const void*,
             std::uint32_t schemaHandle,
             middleware::bap::activity_message::wire_schema::runtime::SchemaView&) noexcept;
using ResolveAdditionalField =
    bool (*)(const void*,
             std::uint32_t fieldRow,
             middleware::bap::activity_message::wire_schema::runtime::FieldView&,
             std::uint32_t& nestedHandle) noexcept;

/** One server process retains at most this many live channel-2 session mirrors. */
inline constexpr std::size_t kCompositeEntitySessionCapacity = 16;

/** One committed channel-2 identity used to resolve update-only records. */
struct EntityBaselineSlot final {
    std::uint8_t allocationEpoch{};
    bool hasAllocationEpoch{};
    std::uint64_t allocationDomain{};
    std::uint64_t serialDomain{};
    std::uint32_t rsatTag{};
    std::uint8_t allocationSequence{};
    std::uint8_t incarnation{};
    EntityType type{EntityType::sobject};
    bool occupied{};
    /** Tombstones retain lifetime and packet ordering after removal. */
    bool known{};
    bool hasPacketSequence{};
    std::uint16_t packetSequence{};
    std::uint64_t packetOrdinal{};
    bool sobjectPlacement{true};
    EntityToken anchor{};
    bool anchorPresent{};
    std::uint64_t anchorOrder{};
};

/** Caller-owned state is scoped to one peer session and simulation view. */
struct EntityBaselineRegistry final {
    std::uint8_t allocationEpoch{};
    bool hasAllocationEpoch{};
    std::uint64_t allocationDomain{};
    state::activity_sdk::Snapshot catalog{};
    std::array<EntityBaselineSlot, kMaximumEntitySlot + 1U> slots{};
    std::uint64_t anchorOrder{};
};

/** One compare-and-commit registry change staged from an accepted batch. */
struct EntityBaselineChange {
    EntityBaselineSlot expected{};
    EntityBaselineSlot replacement{};
    std::uint16_t slot{};
};
struct EntityBaselineMutation final : EntityBaselineChange {
    bool valid{};
    bool hasChanges{};
    std::bitset<kEntityBatchCapacity> ignoredRecordMask{};
    std::uint8_t expectedAllocationEpoch{}, replacementAllocationEpoch{};
    bool expectedHasAllocationEpoch{}, replacementHasAllocationEpoch{};
    std::uint64_t expectedAllocationDomain{}, replacementAllocationDomain{};
    state::gameplay::entity_identity::Source source{};
    bool scoped{};
    std::array<EntityBaselineChange, kEntityBatchCapacity - 1> additionalChanges{};
    std::uint16_t additionalChangeCount{};
    /** Descendants this batch detached because an ancestor received a record and they did not. */
    std::uint16_t detachedCount{};
    std::uint64_t expectedAnchorOrder{}, replacementAnchorOrder{};
};

/** Stable caller-owned dependencies for the composite payload callbacks. */
struct CompositeEntityCodecContext final {
    state::activity_sdk::Snapshot catalog{};
    std::span<const state::activity_sdk::format::EntityTypeDefinition> entityTypes{};
    std::span<const state::activity_sdk::format::SobjectRsat> sobjectRsats{};
    std::span<const state::activity_sdk::format::SobjectRsatDescriptor> sobjectDescriptors{};
    std::span<const state::activity_sdk::format::RsatSchema> rsatSchemas{};
    std::span<const state::activity_sdk::format::RsatField> rsatFields{};
    std::span<const state::activity_sdk::format::SobjectRsatFieldBinding> sobjectBindings{};
    std::span<const state::activity_sdk::format::RuntimeSchema> runtimeSchemas{};
    std::span<const state::activity_sdk::format::RuntimeField> runtimeFields{};
    std::span<const state::activity_sdk::format::RuntimeTypeDefinition> runtimeTypes{};
    EntityBaselineRegistry* registry{};
    SobjectPositionCompression positionCompression{SobjectPositionCompression::disabled};
    bool ready{};
    PositionProfile positionProfile{};
    ResolvePositionProfile resolvePosition{};
    const void* positionContext{};
    state::gameplay::entity_identity::Source source{};
    ResolveSobjectPlan resolvePlan{};
    ResolveSobjectSchemaLayout resolveSchemaLayout{};
    ResolveSobjectFieldLayout resolveFieldLayout{};
    ResolveAdditionalSchema resolveAdditionalSchema{};
    ResolveAdditionalField resolveAdditionalField{};
    const void* planContext{};
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
    /** Offline replay may inspect a refused schema without publishing diagnostic runtime state. */
    void (*schemaFailure)(
        std::uint32_t schema,
        std::uint32_t field,
        const middleware::bap::activity_message::wire_schema::RuntimeDecodeResult&,
        const middleware::bap::activity_message::wire_schema::RuntimeDecodedValue*) noexcept {};
    void (*schemaValues)(
        std::uint32_t schema,
        std::span<
            const middleware::bap::activity_message::wire_schema::RuntimeDecodedValue>) noexcept {};
#endif
};

#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Synthetic row views are accepted only by focused test binaries. */
struct CompositeEntityCatalogFixture final {
    std::span<const state::activity_sdk::format::EntityTypeDefinition> entityTypes{};
    std::span<const state::activity_sdk::format::SobjectRsat> sobjectRsats{};
    std::span<const state::activity_sdk::format::SobjectRsatDescriptor> sobjectDescriptors{};
    std::span<const state::activity_sdk::format::RsatSchema> rsatSchemas{};
    std::span<const state::activity_sdk::format::RsatField> rsatFields{};
    std::span<const state::activity_sdk::format::SobjectRsatFieldBinding> sobjectBindings{};
    std::span<const state::activity_sdk::format::RuntimeSchema> runtimeSchemas{};
    std::span<const state::activity_sdk::format::RuntimeField> runtimeFields{};
    std::span<const state::activity_sdk::format::RuntimeTypeDefinition> runtimeTypes{};
    ResolveSobjectPlan resolvePlan{};
    const void* planContext{};
};

/** Builds a codec over deterministic in-memory v32 rows without publishing a pack. */
[[nodiscard]] bool
initialize_composite_entity_codec_for_test(CompositeEntityCodecContext& context,
                                           EntityBaselineRegistry& registry,
                                           const CompositeEntityCatalogFixture& fixture,
                                           SobjectPositionCompression positionCompression) noexcept;
#endif

/** One session binds its own token baseline space to the shared immutable catalog. */
struct CompositeEntitySession final {
    EntityBaselineRegistry registry{};
    CompositeEntityCodecContext codec{};
    std::uint64_t groupSessionId{};
    state::gameplay::entity_identity::Source source{};
    bool occupied{};
};

/** Bounded mirror state for all channel-2 sessions owned by one gameplay runtime. */
struct CompositeEntitySessionStore final {
    state::activity_sdk::Snapshot catalog{};
    std::array<CompositeEntitySession, kCompositeEntitySessionCapacity> sessions{};
    SobjectPositionCompression positionCompression{SobjectPositionCompression::disabled};
    PositionProfile positionProfile{};
    ResolvePositionProfile resolvePosition{};
    const void* positionContext{};
    ResolveSobjectPlan resolvePlan{};
    ResolveSobjectSchemaLayout resolveSchemaLayout{};
    ResolveSobjectFieldLayout resolveFieldLayout{};
    ResolveAdditionalSchema resolveAdditionalSchema{};
    ResolveAdditionalField resolveAdditionalField{};
    const void* planContext{};
};

/** Pins and validates the current v32 entity reflection catalog. */
[[nodiscard]] bool
initialize_composite_entity_codec(CompositeEntityCodecContext& context,
                                  EntityBaselineRegistry& registry,
                                  SobjectPositionCompression positionCompression) noexcept;

/** Binds one already authenticated SDK snapshot without changing global publication. */
[[nodiscard]] bool initialize_composite_entity_codec_from_catalog(
    CompositeEntityCodecContext& context,
    EntityBaselineRegistry& registry,
    const state::activity_sdk::Snapshot& catalog,
    SobjectPositionCompression positionCompression) noexcept;

/** Builds a mirror-only type payload codec over stable caller-owned context. */
[[nodiscard]] TypePayloadCodec
make_composite_entity_payload_codec(const CompositeEntityCodecContext& context) noexcept;

/** Decodes one retained typed mirror through its published executable schema. */
[[nodiscard]] bool decode_composite_entity_payload(
    const state::activity_sdk::Snapshot& catalog,
    EntityType type,
    TypePayloadPart part,
    const TypePayload& payload,
    std::span<middleware::bap::activity_message::wire_schema::RuntimeDecodedValue> values,
    middleware::bap::activity_message::wire_schema::RuntimeDecodeResult& result) noexcept;

/** Loads the RSAT tag from a validated composite SObject create mirror. */
[[nodiscard]] bool composite_sobject_rsat(const TypePayload& payload,
                                          std::uint32_t& output) noexcept;

/** Validates one decoded batch and stages its single registry change. */
[[nodiscard]] bool stage_entity_baseline_mutation(const CompositeEntityCodecContext& context,
                                                  const EntityBatch& batch,
                                                  EntityBaselineMutation& output) noexcept;

/** Commits a staged change only when its source registry state is unchanged. */
[[nodiscard]] bool commit_accepted_entity_batch(EntityBaselineRegistry& registry,
                                                const EntityBaselineMutation& mutation) noexcept;

/** Resets large stores in place without a stack-sized temporary. */
void reset_composite_entity_sessions(CompositeEntitySessionStore& store) noexcept;

/** Pins the shared catalog used by every later session in this store. */
[[nodiscard]] bool
initialize_composite_entity_sessions(CompositeEntitySessionStore& store,
                                     SobjectPositionCompression positionCompression) noexcept;

/** Reads one batch against the exact session's committed update-only baselines. */
[[nodiscard]] bool read_composite_entity_batch(CompositeEntitySessionStore& store,
                                               std::uint64_t groupSessionId,
                                               middleware::encoding::bits::Reader& reader,
                                               EntityBatch& output) noexcept;

/** Peer accepted-record adapter. It stages and commits only for the named session. */
[[nodiscard]] bool accept_composite_entity_batch(const void* context,
                                                 std::uint64_t groupSessionId,
                                                 const EntityBatch& batch) noexcept;

/** Removes all token baselines owned by one ended group session. */
void reset_composite_entity_session(CompositeEntitySessionStore& store,
                                    std::uint64_t groupSessionId) noexcept;

/** Removes baselines for only the exact retired source. */
void reset_scoped_entity_session(CompositeEntitySessionStore& store,
                                 const state::gameplay::entity_identity::Source& source) noexcept;

/** Delivered retirement preserves tombstones and never touches a newer allocation. */
[[nodiscard]] std::size_t retire_scoped_entity_baselines(
    CompositeEntitySessionStore& store,
    const state::gameplay::entity_identity::Source& source,
    std::span<const state::gameplay::entity_identity::RetiredLifetime> lifetimes) noexcept;

/** A host epoch resets native serial admission without discarding prior lifetime evidence. */
[[nodiscard]] bool
advance_scoped_entity_epoch(CompositeEntitySessionStore& store,
                            const state::gameplay::entity_identity::Source& source,
                            std::uint8_t expected,
                            std::uint8_t next,
                            std::uint64_t nextDomain) noexcept;

/** Scoped production entry points isolate peer channels and view generations. */
[[nodiscard]] bool
prepare_scoped_entity_batch(CompositeEntitySessionStore& store,
                            const state::gameplay::entity_identity::Source& source,
                            const EntityBatch& batch,
                            std::uint16_t packetSequence,
                            bool hasPacketSequence,
                            std::uint64_t packetOrdinal,
                            EntityBaselineMutation& output) noexcept;
[[nodiscard]] bool
commit_scoped_entity_batch(CompositeEntitySessionStore& store,
                           const state::gameplay::entity_identity::Source& source,
                           const EntityBaselineMutation& mutation) noexcept;

/** Scoped production entry points isolate peer channels and view generations. */
[[nodiscard]] bool read_scoped_entity_batch(CompositeEntitySessionStore& store,
                                            const state::gameplay::entity_identity::Source& source,
                                            middleware::encoding::bits::Reader& reader,
                                            EntityBatch& output) noexcept;
[[nodiscard]] bool
accept_scoped_entity_batch(CompositeEntitySessionStore& store,
                           const state::gameplay::entity_identity::Source& source,
                           const EntityBatch& batch) noexcept;

} // namespace sunrise::middleware::gameplay::external
