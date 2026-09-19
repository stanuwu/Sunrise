#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>

#include "../../middleware/gameplay/external/actor_command_runtime_codec.h"
#include "../../middleware/gameplay/external/composite_entity_codec.h"
#include "../../middleware/gameplay/external/simulation_event_runtime_codec.h"
#include "../../state/gameplay/external/rsat_decode_plans.h"
#include "actor_command_policy.h"
#include "actor_command_policy_internal.h"
#include "entity_identities.h"
#include "entity_position_profile_provider.h"
#include "gameplay_log.h"

namespace sunrise::server::gameplay::actor_command_policy {
namespace {

namespace external = middleware::gameplay::external;
namespace wire = middleware::bap::activity_message::wire_schema;

SRWLOCK g_transportLock = SRWLOCK_INIT;
external::CompositeEntitySessionStore g_entitySessions{};
state::gameplay::rsat_decode_plans::Cache g_entityPlans;
bool g_entityIngressReady{};
volatile LONG g_lane0DecodeFailures{};

/** The installed plan cache must match the admitted SDK and its extraction manifest. */
[[nodiscard]] bool load_entity_plans() noexcept {
    namespace plans = state::gameplay::rsat_decode_plans;
    try {
        if (g_entitySessions.catalog == nullptr) {
            return false;
        }
        plans::Digest sdk{};
        const auto identity = g_entitySessions.catalog->sdk_build_sha256();
        if (identity.size() != sdk.size()) {
            return false;
        }
        std::copy(identity.begin(), identity.end(), sdk.begin());
        if (!g_entityPlans.load_installed(g_entitySessions.catalog->artifact_directory(), sdk)) {
            return false;
        }
        g_entitySessions.resolvePlan = plans::resolve_plan;
        g_entitySessions.resolveSchemaLayout = plans::resolve_schema_layout;
        g_entitySessions.resolveFieldLayout = plans::resolve_field_layout;
        g_entitySessions.resolveAdditionalSchema = plans::resolve_additional_schema;
        g_entitySessions.resolveAdditionalField = plans::resolve_additional_field;
        g_entitySessions.planContext = &g_entityPlans;
        return true;
    } catch (...) {
        return false;
    }
}

/** Validates one reflected event body against the current immutable SDK catalog. */
[[nodiscard]] bool read_event_payload(const void*,
                                      std::uint8_t eventType,
                                      external::SimulationEventPayloadPart part,
                                      const external::SimulationEventPayloadView* primary,
                                      middleware::encoding::bits::Reader& reader) noexcept {
    external::ActorCommandCatalog catalog{};
    if (!external::published_actor_command_catalog(catalog)) {
        return false;
    }
    const external::RuntimeEventPayloadCodecContext context{&catalog};
    const external::SimulationEventPayloadCodec codec =
        external::make_runtime_event_payload_codec(context);
    if (codec.read(codec.context, eventType, part, primary, reader)) {
        return true;
    }
    if (InterlockedIncrement(&g_lane0DecodeFailures) <= 32) {
        std::uint32_t schema = state::activity_sdk::format::kAbsentIndex;
        for (const state::activity_sdk::format::SimulationEventDefinition& event : catalog.events) {
            if (event.eventType == eventType) {
                schema = part == external::SimulationEventPayloadPart::primary
                             ? event.primarySchema
                             : event.secondarySchema;
                break;
            }
        }
        wire::RuntimeDecodeResult decoded{};
        std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
        middleware::encoding::bits::Reader diagnosticReader = reader;
        const wire::RuntimeSchemaResolver resolver =
            external::actor_runtime_schema_resolver(catalog);
        const bool root =
            schema != state::activity_sdk::format::kAbsentIndex
            && wire::decode_full_schema_prefix(schema, diagnosticReader, resolver, values, decoded);
        std::uint32_t selectedSchema = state::activity_sdk::format::kAbsentIndex;
        if (primary != nullptr) {
            middleware::encoding::bits::Reader primaryReader(primary->arena);
            wire::RuntimeDecodeResult primaryDecoded{};
            std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> primaryValues{};
            const auto event =
                std::find_if(catalog.events.begin(),
                             catalog.events.end(),
                             [eventType](const auto& row) { return row.eventType == eventType; });
            if (event != catalog.events.end() && primaryReader.skip(primary->payload.bitOffset)
                && wire::decode_full_schema_prefix(
                    event->primarySchema, primaryReader, resolver, primaryValues, primaryDecoded)) {
                for (std::size_t index = 0; index < primaryDecoded.valueCount; ++index) {
                    if (primaryValues[index].present
                        && primaryValues[index].role == wire::ValueRole::schemaReference
                        && primaryValues[index].unsignedValue
                               <= (std::numeric_limits<std::uint32_t>::max)()) {
                        selectedSchema =
                            static_cast<std::uint32_t>(primaryValues[index].unsignedValue);
                        break;
                    }
                }
            }
        }
        wire::RuntimeDecodeResult selectedDecoded{};
        std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> selectedValues{};
        const bool selected =
            root && selectedSchema != state::activity_sdk::format::kAbsentIndex
            && wire::decode_full_schema_prefix(
                selectedSchema, diagnosticReader, resolver, selectedValues, selectedDecoded);
        std::uint32_t commandSelector = state::activity_sdk::format::kAbsentIndex;
        std::uint32_t commandPayload = state::activity_sdk::format::kAbsentIndex;
        std::array<std::uint32_t, 2> variantSelectors{state::activity_sdk::format::kAbsentIndex,
                                                      state::activity_sdk::format::kAbsentIndex};
        std::size_t variantCount = 0;
        for (std::size_t index = 0; index < selectedDecoded.valueCount; ++index) {
            if (selectedValues[index].role == wire::ValueRole::variantSelector
                && selectedValues[index].unsignedValue
                       <= (std::numeric_limits<std::uint32_t>::max)()
                && variantCount < variantSelectors.size()) {
                variantSelectors[variantCount++] =
                    static_cast<std::uint32_t>(selectedValues[index].unsignedValue);
            }
            if (selectedValues[index].role == wire::ValueRole::commandSelector
                && selectedValues[index].unsignedValue
                       <= (std::numeric_limits<std::uint32_t>::max)()) {
                commandSelector = static_cast<std::uint32_t>(selectedValues[index].unsignedValue);
                break;
            }
        }
        if (commandSelector < catalog.commands.size()
            && catalog.commands[commandSelector].selector == commandSelector) {
            commandPayload = catalog.commands[commandSelector].payloadHandle;
        }
        report(core::log::Level::debug,
               "ev=actor_policy stage=lane0_decode result=fail event=%u part=%u schema=0x%08X "
               "root=%u status=%u consumed=%zu remaining=%zu values=%zu truncated=%u "
               "selected_schema=0x%08X selected=%u selected_status=%u selected_consumed=%zu "
               "selected_values=%zu variant0=%u variant1=%u command=%u "
               "command_payload=0x%08X",
               static_cast<unsigned>(eventType),
               part == external::SimulationEventPayloadPart::primary ? 0U : 1U,
               static_cast<unsigned>(schema),
               root ? 1U : 0U,
               static_cast<unsigned>(decoded.status),
               decoded.bitsConsumed,
               decoded.bitsRemaining,
               decoded.valueCount,
               decoded.valuesTruncated ? 1U : 0U,
               static_cast<unsigned>(selectedSchema),
               selected ? 1U : 0U,
               static_cast<unsigned>(selectedDecoded.status),
               selectedDecoded.bitsConsumed,
               selectedDecoded.valueCount,
               static_cast<unsigned>(variantSelectors[0]),
               static_cast<unsigned>(variantSelectors[1]),
               static_cast<unsigned>(commandSelector),
               static_cast<unsigned>(commandPayload));
    }
    return false;
}

/** Commits one accepted lane only to the named group policy. */
[[nodiscard]] bool accepted_lane0_adapter(const void*,
                                          std::uint64_t groupSessionId,
                                          const external::SimulationEventBatch& batch) noexcept {
    return accept_lane0(groupSessionId, batch);
}

/** Binds one encoded lane to the packet's exact transmission identity. */
[[nodiscard]] bool write_lane0_adapter(const void*,
                                       std::uint64_t groupSessionId,
                                       std::uint64_t transmissionId,
                                       middleware::encoding::bits::Writer& writer) noexcept {
    return write_lane0(groupSessionId, transmissionId, writer);
}

/** Applies one exact packet outcome without folding another transmission into it. */
void lane0_outcome_adapter(const void*,
                           std::uint64_t groupSessionId,
                           std::uint64_t transmissionId,
                           middleware::gameplay::peer::AckOutcome outcome) noexcept {
    lane0_outcome(groupSessionId, transmissionId, outcome);
}

/** Clears transport-scoped actor state while preserving the durable policy. */
void reset_lane0_adapter(const void*, std::uint64_t groupSessionId) noexcept {
    internal::reset_transport_session(groupSessionId);
}

/** Decodes channel 2 against only the named group's committed baselines. */
[[nodiscard]] bool read_entity_adapter(const void* context,
                                       const state::gameplay::entity_identity::Source& source,
                                       middleware::encoding::bits::Reader& reader,
                                       external::EntityBatch& batch) noexcept {
    if (context == nullptr) {
        return false;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    const bool accepted = external::read_scoped_entity_batch(store, source, reader, batch);
    ReleaseSRWLockExclusive(&g_transportLock);
    return accepted;
}

/** Preflight must finish before any other lane commits. */
[[nodiscard]] bool prepare_entity_adapter(const void* context,
                                          const state::gameplay::entity_identity::Source& source,
                                          const external::EntityBatch& batch,
                                          std::uint16_t sequence,
                                          bool hasSequence,
                                          std::uint64_t ordinal,
                                          external::EntityBaselineMutation& mutation) noexcept {
    if (context == nullptr) {
        return false;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    const bool accepted = external::prepare_scoped_entity_batch(
        store, source, batch, sequence, hasSequence, ordinal, mutation);
    ReleaseSRWLockExclusive(&g_transportLock);
    if (accepted && mutation.detachedCount != 0) {
        report(core::log::Level::debug,
               "ev=entity_identity stage=anchor_prune detached=%u records=%zu",
               static_cast<unsigned>(mutation.detachedCount),
               external::entity_record_count(batch));
    }
    return accepted;
}

/** The peer lock prevents source changes between preflight and commit. */
[[nodiscard]] bool
commit_entity_adapter(const void* context,
                      const state::gameplay::entity_identity::Source& source,
                      const external::EntityBaselineMutation& mutation) noexcept {
    if (context == nullptr) {
        return false;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    const bool accepted = external::commit_scoped_entity_batch(store, source, mutation);
    ReleaseSRWLockExclusive(&g_transportLock);
    return accepted;
}

/** Observers run only after the complete external packet's acceptance boundary. */
void observed_entity_adapter(const void* context,
                             const state::gameplay::entity_identity::Source& source,
                             const external::EntityBatch& batch,
                             std::uint16_t packetSequence,
                             bool hasPacketSequence,
                             std::uint64_t ordinal,
                             std::uint64_t tick) noexcept {
    if (context == nullptr) {
        return;
    }
    AcquireSRWLockShared(&g_transportLock);
    const auto catalog =
        static_cast<const external::CompositeEntitySessionStore*>(context)->catalog;
    ReleaseSRWLockShared(&g_transportLock);
    entity_identities::observe(source,
                               batch,
                               catalog,
                               packetSequence,
                               hasPacketSequence,
                               ordinal,
                               tick,
                               batch.allocationEpoch,
                               batch.hasAllocationEpoch,
                               batch.allocationDomain);
    const external::EntityBatch* projected = &batch;
    std::unique_ptr<external::EntityBatch> filtered;
    if (batch.ignoredRecordMask.any()) {
        filtered.reset(new (std::nothrow) external::EntityBatch{});
        if (!filtered) {
            return;
        }
        *filtered = batch;
        std::size_t count = 0;
        for (std::size_t index = 0; index < external::entity_record_count(batch); ++index) {
            if (!batch.ignoredRecordMask.test(index)) {
                external::entity_record_at(*filtered, count++) =
                    external::entity_record_at(batch, index);
            }
        }
        if (count == 0) {
            return;
        }
        filtered->recordPresent = true;
        filtered->additionalRecordCount = static_cast<std::uint16_t>(count - 1);
        filtered->ignoredRecordMask.reset();
        projected = filtered.get();
    }
    if (!accept_entity_batch(source, *projected)) {
        report(core::log::Level::debug,
               "ev=entity_identity stage=actor_projection result=unavailable");
    }
}

/** Host epoch changes reset serial admission without erasing retained allocation evidence. */
void advance_entity_epoch_adapter(const void* context,
                                  const state::gameplay::entity_identity::Source& source,
                                  std::uint8_t expected,
                                  std::uint8_t next,
                                  std::uint64_t domain) noexcept {
    if (context == nullptr) {
        return;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    const bool advanced =
        external::advance_scoped_entity_epoch(store, source, expected, next, domain);
    ReleaseSRWLockExclusive(&g_transportLock);
    if (!advanced) {
        report(core::log::Level::debug, "ev=entity_identity stage=allocation_epoch result=stale");
    }
}

/** Delivered retirements cannot erase a replacement that reused the same network slot. */
std::size_t retire_entity_adapter(
    const void* context,
    const state::gameplay::entity_identity::Source& source,
    std::span<const state::gameplay::entity_identity::RetiredLifetime> lifetimes) noexcept {
    if (context == nullptr) {
        return 0;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    const auto retired = external::retire_scoped_entity_baselines(store, source, lifetimes);
    ReleaseSRWLockExclusive(&g_transportLock);
    return retired;
}

/** Removes every update-only baseline owned by one replaced group view. */
void reset_entity_adapter(const void* context,
                          const state::gameplay::entity_identity::Source& source) noexcept {
    if (context == nullptr) {
        return;
    }
    auto& store = *const_cast<external::CompositeEntitySessionStore*>(
        static_cast<const external::CompositeEntitySessionStore*>(context));
    AcquireSRWLockExclusive(&g_transportLock);
    external::reset_scoped_entity_session(store, source);
    ReleaseSRWLockExclusive(&g_transportLock);
    entity_identities::reset_source(source);
}

} // namespace

bool internal::entity_transport_ready() noexcept {
    AcquireSRWLockShared(&g_transportLock);
    const bool ready = g_entityIngressReady;
    ReleaseSRWLockShared(&g_transportLock);
    return ready;
}

void internal::shutdown_entity_transport() noexcept {
    peer::install_entity_transport({});
    peer::install_entity_codec({}, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_transportLock);
    g_entityIngressReady = false;
    external::reset_composite_entity_sessions(g_entitySessions);
    ReleaseSRWLockExclusive(&g_transportLock);
    entity_identities::reset();
}

/** Installs one bounded baseline store shared only through session-aware callbacks. */
bool install_entity_transport() noexcept {
    (void)InterlockedExchange(&g_lane0DecodeFailures, 0);
    peer::install_entity_transport({});
    entity_identities::reset();
    AcquireSRWLockExclusive(&g_transportLock);
    g_entityIngressReady = false;
    const bool initialized = external::initialize_composite_entity_sessions(
                                 g_entitySessions, external::SobjectPositionCompression::disabled)
                             && load_entity_plans();
    if (initialized) {
        g_entitySessions.resolvePosition = &resolve_entity_position_profile;
        g_entitySessions.positionContext = nullptr;
    }
    ReleaseSRWLockExclusive(&g_transportLock);
    if (!initialized) {
        return false;
    }
    peer::install_lane0_transport(lane0_transport());
    peer::EntityTransport transport{};
    transport.context = &g_entitySessions;
    transport.read = &read_entity_adapter;
    transport.prepare = &prepare_entity_adapter;
    transport.commit = &commit_entity_adapter;
    transport.reset = &reset_entity_adapter;
    transport.retire = &retire_entity_adapter;
    transport.advanceEpoch = &advance_entity_epoch_adapter;
    transport.observed = &observed_entity_adapter;
    peer::install_entity_transport(transport);
    peer::install_entity_codec({}, nullptr, nullptr);
    AcquireSRWLockExclusive(&g_transportLock);
    g_entityIngressReady = true;
    ReleaseSRWLockExclusive(&g_transportLock);
    report(core::log::Level::info, "ev=entity_identity stage=transport result=ready");
    return true;
}

external::SimulationEventPayloadCodec lane0_payload_codec() noexcept {
    external::SimulationEventPayloadCodec codec{};
    codec.read = &read_event_payload;
    codec.maximumPrimaryBits = external::kMaximumSimulationEventPayloadBits;
    codec.maximumSecondaryBits = external::kMaximumSimulationEventPayloadBits;
    return codec;
}

peer::Lane0Transport lane0_transport() noexcept {
    peer::Lane0Transport transport{};
    transport.payloadCodec = lane0_payload_codec();
    transport.accepted = &accepted_lane0_adapter;
    transport.write = &write_lane0_adapter;
    transport.outcome = &lane0_outcome_adapter;
    transport.reset = &reset_lane0_adapter;
    return transport;
}

/** Decodes one lane through the currently published SDK event catalog. */
bool decode_lane0(middleware::encoding::bits::Reader& reader,
                  external::SimulationEventBatch& output) noexcept {
    external::ActorCommandCatalog catalog{};
    if (!external::published_actor_command_catalog(catalog)) {
        return false;
    }
    const external::RuntimeEventPayloadCodecContext context{&catalog};
    const external::SimulationEventPayloadCodec codec =
        external::make_runtime_event_payload_codec(context);
    return external::read_simulation_event_lane(reader, codec, output);
}

} // namespace sunrise::server::gameplay::actor_command_policy
