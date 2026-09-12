#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../middleware/gameplay/external/composite_entity_codec.h"
#include "../../middleware/gameplay/external/simulation_event_runtime_codec.h"
#include "../../state/activity_sdk/runtime.h"
#include "../activity/mission/mission_script_runtime.h"
#include "actor_command_policy.h"
#include "actor_command_policy_internal.h"
#include "actor_command_policy_session.h"
#include "gameplay_log.h"

namespace sunrise::server::gameplay::actor_command_policy {

namespace external = middleware::gameplay::external;
namespace format = state::activity_sdk::format;
namespace wire = middleware::bap::activity_message::wire_schema;

namespace {

volatile LONG g_entityRecordDiagnostics{};

/** Decoded layout addressed by the type-1 baseline schema fields. */
struct SquadClientRefLayout final {
    std::uint32_t registryKey{};
    std::int8_t slotType{};
    std::byte alignment{};
    std::int16_t slotIndex{};
};

/** @return A decoded integral value as a signed scalar. */
[[nodiscard]] bool integral_value(const wire::RuntimeDecodedValue& value,
                                  std::int64_t& output) noexcept {
    if (!value.present) {
        return false;
    }
    if (value.kind == wire::ValueKind::signedInteger) {
        output = value.signedValue;
        return true;
    }
    if (value.kind == wire::ValueKind::unsignedInteger
        && value.unsignedValue
               <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        output = static_cast<std::int64_t>(value.unsignedValue);
        return true;
    }
    return false;
}

/** Decodes the type-1 baseline ClientRef triple. */
[[nodiscard]] bool decode_squad_identity(const state::activity_sdk::Snapshot& catalog,
                                         const external::TypePayload& payload,
                                         SquadEntityRow& output) noexcept {
    std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
    wire::RuntimeDecodeResult result{};
    if (!external::decode_composite_entity_payload(catalog,
                                                   external::EntityType::squad,
                                                   external::TypePayloadPart::baseline,
                                                   payload,
                                                   values,
                                                   result)) {
        return false;
    }
    bool key = false;
    bool type = false;
    bool index = false;
    for (std::size_t position = 0; position < result.valueCount; ++position) {
        const wire::RuntimeDecodedValue& value = values[position];
        if (value.fieldRow >= catalog->runtime_fields().size()) {
            return false;
        }
        const std::uint64_t offset = catalog->runtime_fields()[value.fieldRow].structOffset;
        std::int64_t decoded = 0;
        if (value.role != wire::ValueRole::scalar || !integral_value(value, decoded)) {
            continue;
        }
        if (offset == offsetof(SquadClientRefLayout, registryKey) && decoded > 0
            && decoded <= (std::numeric_limits<std::uint32_t>::max)()) {
            output.registryKey = static_cast<std::uint32_t>(decoded);
            key = true;
        } else if (offset == offsetof(SquadClientRefLayout, slotType) && decoded >= 0
                   && decoded <= (std::numeric_limits<std::uint8_t>::max)()) {
            output.slotType = static_cast<std::uint8_t>(decoded);
            type = true;
        } else if (offset == offsetof(SquadClientRefLayout, slotIndex) && decoded >= 0
                   && decoded <= (std::numeric_limits<std::uint32_t>::max)()) {
            output.slotIndex = static_cast<std::uint32_t>(decoded);
            index = true;
        }
    }
    return key && type && index;
}

/** Decodes the latest actor-token list from one type-1 update. */
[[nodiscard]] bool decode_squad_actors(const state::activity_sdk::Snapshot& catalog,
                                       const external::TypePayload& payload,
                                       SquadEntityRow& output) noexcept {
    std::array<wire::RuntimeDecodedValue, wire::kRuntimeValueCapacity> values{};
    wire::RuntimeDecodeResult result{};
    if (!external::decode_composite_entity_payload(catalog,
                                                   external::EntityType::squad,
                                                   external::TypePayloadPart::update,
                                                   payload,
                                                   values,
                                                   result)) {
        return false;
    }
    output.actorCount = 0;
    for (std::size_t position = 0; position < result.valueCount; ++position) {
        const wire::RuntimeDecodedValue& slot = values[position];
        if (!slot.present || slot.role != wire::ValueRole::entityReferenceSlot
            || slot.unsignedValue > external::kMaximumEntitySlot) {
            continue;
        }
        const auto end = values.begin() + static_cast<std::ptrdiff_t>(result.valueCount);
        const auto incarnation =
            std::find_if(values.begin() + static_cast<std::ptrdiff_t>(position) + 1,
                         end,
                         [&slot](const wire::RuntimeDecodedValue& value) {
                             return value.present && value.fieldRow == slot.fieldRow
                                    && value.occurrence == slot.occurrence
                                    && value.role == wire::ValueRole::entityReferenceIncarnation;
                         });
        if (incarnation == end || incarnation->unsignedValue > external::kMaximumEntityIncarnation
            || output.actorCount == output.actors.size()) {
            return false;
        }
        output.actors[output.actorCount++] = {
            static_cast<std::uint16_t>(slot.unsignedValue),
            static_cast<std::uint8_t>(incarnation->unsignedValue)};
    }
    return true;
}

/** @return True when the active policy names this exact live squad. */
[[nodiscard]] bool selected_squad(const SessionRow& session, const SquadEntityRow& squad) noexcept {
    return std::any_of(session.selectedSquads.begin(),
                       session.selectedSquads.begin() + session.selectedSquadCount,
                       [&squad](const SelectedSquad& selected) {
                           return selected.registryKey == squad.registryKey
                                  && selected.slotType == squad.slotType
                                  && selected.slotIndex == squad.slotIndex;
                       });
}

/** Applies one type-1 record and queues policy for its current exact members. */
[[nodiscard]] bool accept_squad_record(SessionRow& session,
                                       const external::EntityRecord& record) noexcept {
    auto row =
        std::find_if(session.squads.begin(), session.squads.end(), [&record](const auto& value) {
            return value.occupied
                   && (((record.flags & external::entityCreate) != 0
                        && value.token.slot == record.token.slot)
                       || same_token(value.token, record.token));
        });
    if ((record.flags & external::entityRemove) != 0) {
        if (row != session.squads.end()) {
            const auto prior = *row;
            *row = {};
            for (std::size_t index = 0; index < prior.actorCount; ++index) {
                std::uint32_t actorClass = format::kAbsentIndex;
                if (!selected_entity_class(session, prior.actors[index], actorClass)) {
                    remove_target_state(session, prior.actors[index]);
                }
            }
        }
        return true;
    }
    SquadEntityRow candidate =
        row == session.squads.end() || (record.flags & external::entityCreate) != 0
            ? SquadEntityRow{}
            : *row;
    candidate.token = record.token;
    candidate.occupied = true;
    if ((record.flags & external::entityCreate) != 0
        && !decode_squad_identity(session.catalog, record.baseline, candidate)) {
        return false;
    }
    if ((record.flags & external::entityUpdate) != 0
        && !decode_squad_actors(session.catalog, record.update, candidate)) {
        return false;
    }
    if (row == session.squads.end()) {
        row = std::find_if(session.squads.begin(), session.squads.end(), [](const auto& value) {
            return !value.occupied;
        });
        if (row == session.squads.end()) {
            return false;
        }
    }
    const SquadEntityRow prior = *row;
    std::array<bool, kSquadActorCapacity> priorSelected{};
    for (std::size_t index = 0; index < candidate.actorCount; ++index) {
        std::uint32_t actorClass = format::kAbsentIndex;
        priorSelected[index] = selected_entity_class(session, candidate.actors[index], actorClass);
    }
    *row = candidate;
    const bool selected = session.policyActive && selected_squad(session, candidate);
    report(core::log::Level::debug,
           "ev=actor_policy stage=squad_record key=0x%08X type=%u index=%u actors=%u "
           "selected=%u flags=0x%X",
           candidate.registryKey,
           static_cast<unsigned>(candidate.slotType),
           static_cast<unsigned>(candidate.slotIndex),
           static_cast<unsigned>(candidate.actorCount),
           selected ? 1U : 0U,
           static_cast<unsigned>(record.flags));
    if (!selected) {
        for (std::size_t index = 0; index < prior.actorCount; ++index) {
            std::uint32_t actorClass = format::kAbsentIndex;
            if (!selected_entity_class(session, prior.actors[index], actorClass)) {
                remove_target_state(session, prior.actors[index]);
            }
        }
        return true;
    }
    std::array<external::EntityToken, kSquadActorCapacity> queued{};
    std::size_t queuedCount = 0;
    for (std::size_t index = 0; index < candidate.actorCount; ++index) {
        std::uint32_t actorClass = format::kAbsentIndex;
        if (priorSelected[index]
            || !selected_entity_class(session, candidate.actors[index], actorClass)) {
            continue;
        }
        if (!queue_command(session,
                           actorClass,
                           candidate.actors[index],
                           session.policyValue,
                           OutputPurpose::policyCommand)) {
            for (OutputRow& output : session.outputs) {
                if (output.purpose == OutputPurpose::policyCommand
                    && std::any_of(queued.begin(),
                                   queued.begin() + static_cast<std::ptrdiff_t>(queuedCount),
                                   [&output](const auto& token) {
                                       return same_token(output.target, token);
                                   })) {
                    output = {};
                }
            }
            *row = prior;
            return false;
        }
        queued[queuedCount++] = candidate.actors[index];
    }
    for (std::size_t index = 0; index < prior.actorCount; ++index) {
        std::uint32_t actorClass = format::kAbsentIndex;
        if (!selected_entity_class(session, prior.actors[index], actorClass)) {
            remove_target_state(session, prior.actors[index]);
        }
    }
    return true;
}

} // namespace

/** Optional policy projection retains only its supported entity metadata. */
static bool accept_entity_record(std::uint64_t groupSessionId,
                                 const external::EntityRecord& record) noexcept {
    if (groupSessionId == 0) {
        return false;
    }
    if (InterlockedIncrement(&g_entityRecordDiagnostics) <= 128) {
        report(core::log::Level::debug,
               "ev=actor_policy stage=entity_record type=%u flags=0x%X slot=%u incarnation=%u "
               "baseline=%u update=%u",
               static_cast<unsigned>(record.type),
               static_cast<unsigned>(record.flags),
               static_cast<unsigned>(record.token.slot),
               static_cast<unsigned>(record.token.incarnation),
               static_cast<unsigned>(record.baseline.byteCount),
               static_cast<unsigned>(record.update.byteCount));
    }
    external::ActorEntityCatalog published{};
    if (!external::published_actor_entity_catalog(published)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_or_create_session(groupSessionId);
    if (session == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (session->catalog == nullptr
        || (!session->policyActive && session->catalog != published.owner)) {
        session->catalog = published.owner;
    }
    // A live policy owns its catalog. Records from another one name different class rows.
    if (session->policyActive && session->catalog != published.owner) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (record.type == external::EntityType::squad) {
        const bool accepted = accept_squad_record(*session, record);
        ReleaseSRWLockExclusive(&g_lock);
        return accepted;
    }
    if (record.type != external::EntityType::sobject) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    external::ActorEntityCatalog catalog{session->catalog, session->catalog->actor_classes()};
    if (record.token.slot >= session->actors.slots.size()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const bool catalogChange = session->actors.catalog != catalog.owner
                               || session->actors.classData != catalog.classes.data()
                               || session->actors.classCount != catalog.classes.size();
    if (catalogChange
        && std::any_of(session->actors.slots.begin(),
                       session->actors.slots.end(),
                       [](const auto& slot) { return slot.occupied; })) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const state::activity_sdk::Snapshot priorCatalog = session->actors.catalog;
    const format::ActorClass* const priorClassData = session->actors.classData;
    const std::size_t priorClassCount = session->actors.classCount;
    const external::ActorEntitySlot priorSlot = session->actors.slots[record.token.slot];
    const external::EntityToken priorToken{record.token.slot, priorSlot.incarnation};
    std::uint32_t priorClass = format::kAbsentIndex;
    const bool priorSelected =
        session->policyActive && selected_entity_class(*session, priorToken, priorClass);
    const auto priorSquad =
        selected_entity_squad(*session, priorToken, session->selectedSquadCount);
    const external::ActorEntityApplyResult result =
        external::apply_actor_entity_record(session->actors, catalog, record);
    bool policyCommandQueued = false;
    const OutputRow* queuedOutput = nullptr;
    bool accepted = result != external::ActorEntityApplyResult::invalid
                    && result != external::ActorEntityApplyResult::staleToken;
    std::uint32_t actorClassIndex = format::kAbsentIndex;
    const bool selected = accepted && session->policyActive
                          && selected_entity_class(*session, record.token, actorClassIndex);
    const auto selectedSquad =
        selected_entity_squad(*session, record.token, session->selectedSquadCount);
    if (selected
        && (result == external::ActorEntityApplyResult::actorCreated || !priorSelected
            || selectedSquad != priorSquad)) {
        const auto empty = std::find_if(
            session->outputs.begin(), session->outputs.end(), [](const OutputRow& output) {
                return output.state == OutputState::empty;
            });
        if (empty != session->outputs.end()) {
            queuedOutput = &*empty;
            policyCommandQueued = queue_command(*session,
                                                actorClassIndex,
                                                record.token,
                                                session->policyValue,
                                                OutputPurpose::policyCommand);
        }
        accepted = policyCommandQueued;
    }
    if (accepted) {
        const bool replaced = result == external::ActorEntityApplyResult::actorCreated
                              || result == external::ActorEntityApplyResult::nonActor
                              || result == external::ActorEntityApplyResult::actorRemoved;
        if (priorSlot.occupied && (replaced || (priorSelected && !selected))) {
            remove_target_state(*session, priorToken, queuedOutput);
        }
        if (result == external::ActorEntityApplyResult::actorRemoved
            || (!selected && session->actors.slots[record.token.slot].authoredSource.known)) {
            remove_target_state(*session, record.token, queuedOutput);
        }
    }
    // The projection is all-or-nothing, so a refused command restores the whole slot.
    if (!accepted) {
        session->actors.catalog = priorCatalog;
        session->actors.classData = priorClassData;
        session->actors.classCount = priorClassCount;
        session->actors.slots[record.token.slot] = priorSlot;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (policyCommandQueued) {
        report(core::log::Level::info,
               "ev=actor_policy stage=command result=queued group=0x%016llX slot=%u incarnation=%u",
               static_cast<unsigned long long>(groupSessionId),
               static_cast<unsigned>(record.token.slot),
               static_cast<unsigned>(record.token.incarnation));
    }
    return accepted;
}

/** Policy projection visits every record after transport acceptance. */
bool accept_entity_batch(std::uint64_t groupSessionId,
                         const external::EntityBatch& batch) noexcept {
    if (groupSessionId == 0
        || external::entity_record_count(batch) > external::kEntityBatchCapacity) {
        return false;
    }
    bool accepted = true;
    for (std::size_t index = 0; index < external::entity_record_count(batch); ++index) {
        if (batch.ignoredRecordMask.test(index)) {
            continue;
        }
        accepted = accept_entity_record(groupSessionId, external::entity_record_at(batch, index))
                   && accepted;
    }
    return accepted;
}

/**
 * Retains all new damage replays transactionally for one exact group session.
 * @param groupSessionId Group session the lane arrived on.
 * @param batch Decoded lane-0 events.
 * @return True when every damage event is retained and its restore command queued.
 */
bool accept_lane0(std::uint64_t groupSessionId,
                  const external::SimulationEventBatch& batch) noexcept {
    if (groupSessionId == 0 || batch.count > batch.records.size()) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_session(groupSessionId);
    if (session == nullptr || !session->policyActive) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    if (session->catalog != state::activity_sdk::snapshot()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    external::ActorCommandCatalog catalog{};
    if (!external::published_actor_command_catalog(session->catalog, catalog)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    bool accepted = true;
    std::uint32_t eventTypes = 0;
    std::array<std::uint32_t, kReplayCapacity> createdReplays{};
    std::size_t createdReplayCount = 0;
    std::array<std::size_t, kReplayCapacity> provokedSquads{};
    std::size_t provokedCount = 0;
    std::array<SelectedSquad, kReplayCapacity> notifications{};
    std::array<OutputRow*, kOutputCapacity> stagedOutputs{};
    std::size_t stagedCount = 0;
    const auto binding = session->binding;
    const auto sourceGeneration = session->activityClientGeneration;
    const auto restore =
        [&](std::uint32_t actorClass, external::EntityToken actor, std::uint32_t replayIndex) {
            auto empty =
                std::find_if(session->outputs.begin(), session->outputs.end(), [](const auto& row) {
                    return row.state == OutputState::empty;
                });
            if (empty == session->outputs.end() || actorClass >= catalog.profiles.size()
                || stagedCount == stagedOutputs.size()
                || !queue_command(*session,
                                  actorClass,
                                  actor,
                                  catalog.profiles[actorClass].defaultFaction,
                                  OutputPurpose::restoreCommand,
                                  replayIndex)) {
                return false;
            }
            stagedOutputs[stagedCount++] = &*empty;
            return true;
        };
    for (std::size_t index = 0; index < batch.count; ++index) {
        external::DecodedRuntimeEvent event{};
        if (!internal::decode_event(catalog, batch, batch.records[index], event)) {
            accepted = false;
            break;
        }
        if (batch.records[index].eventType <= external::kMaximumSimulationEventType) {
            eventTypes |= std::uint32_t{1} << batch.records[index].eventType;
        }
        if (event.identity.eventIndex != session->damageEventIndex) {
            continue;
        }
        external::EntityToken target{};
        std::uint32_t actorClassIndex = format::kAbsentIndex;
        const bool targetPresent = internal::damage_target(event, target);
        if (!targetPresent || !selected_entity_class(*session, target, actorClassIndex)) {
            report(
                core::log::Level::debug,
                "ev=actor_policy stage=damage result=unselected target=%u slot=%u incarnation=%u",
                targetPresent ? 1U : 0U,
                static_cast<unsigned>(target.slot),
                static_cast<unsigned>(target.incarnation));
            continue;
        }
        const auto squad = selected_entity_squad(*session, target, session->selectedSquadCount);
        if (squad >= session->selectedSquadCount || session->selectedSquads[squad].provoked
            || std::find(provokedSquads.begin(),
                         provokedSquads.begin() + static_cast<std::ptrdiff_t>(provokedCount),
                         squad)
                   != provokedSquads.begin() + static_cast<std::ptrdiff_t>(provokedCount)) {
            continue;
        }
        // One replay per target. A second hit on the same actor restores the same faction.
        const auto replay = std::find_if(
            session->replays.begin(), session->replays.end(), [&target](const auto& row) {
                return row.occupied && same_token(row.target, target);
            });
        if (replay != session->replays.end()) {
            continue;
        }
        const auto emptyReplay = std::find_if(session->replays.begin(),
                                              session->replays.end(),
                                              [](const auto& row) { return !row.occupied; });
        if (emptyReplay == session->replays.end()
            || !external::retain_runtime_event(emptyReplay->transaction, event)) {
            accepted = false;
            break;
        }
        emptyReplay->target = target;
        emptyReplay->occupied = true;
        const std::uint32_t replayIndex =
            static_cast<std::uint32_t>(emptyReplay - session->replays.begin());
        createdReplays[createdReplayCount++] = replayIndex;
        if (!restore(actorClassIndex, target, replayIndex)) {
            *emptyReplay = {};
            accepted = false;
            break;
        }
        for (std::size_t slot = 0; slot < session->actors.slots.size(); ++slot) {
            const auto& actor = session->actors.slots[slot];
            const external::EntityToken member{static_cast<std::uint16_t>(slot), actor.incarnation};
            std::uint32_t memberClass = format::kAbsentIndex;
            if (!actor.occupied || same_token(member, target)
                || selected_entity_squad(*session, member, session->selectedSquadCount) != squad
                || !selected_entity_class(*session, member, memberClass)) {
                continue;
            }
            if (!restore(memberClass, member, format::kAbsentIndex)) {
                accepted = false;
                break;
            }
        }
        if (!accepted) {
            break;
        }
        provokedSquads[provokedCount++] = squad;
    }
    // The lane is accepted whole, so a refused event drops every replay this call created.
    if (!accepted) {
        for (std::size_t index = 0; index < stagedCount; ++index) {
            *stagedOutputs[index] = {};
        }
        for (std::size_t index = 0; index < createdReplayCount; ++index) {
            session->replays[createdReplays[index]] = {};
        }
    } else {
        for (std::size_t index = 0; index < provokedCount; ++index) {
            auto& squad = session->selectedSquads[provokedSquads[index]];
            squad.provoked = true;
            notifications[index] = squad;
            for (auto& output : session->outputs) {
                if (output.state != OutputState::empty
                    && output.purpose == OutputPurpose::policyCommand
                    && selected_entity_squad(*session, output.target, session->selectedSquadCount)
                           == provokedSquads[index]) {
                    output = {};
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (accepted) {
        if (batch.count != 0) {
            report(core::log::Level::debug,
                   "ev=actor_policy stage=lane0 result=accepted events=%u types=0x%08X",
                   static_cast<unsigned>(batch.count),
                   eventTypes);
        }
        for (std::size_t index = 0; index < provokedCount; ++index) {
            const auto& squad = notifications[index];
            server::activity::mission::report_squad_provoked(
                binding,
                sourceGeneration,
                squad.registryKey,
                static_cast<std::uint16_t>(squad.slotIndex));
            report(core::log::Level::info,
                   "ev=actor_policy stage=provoked key=0x%08X type=%u index=%u",
                   squad.registryKey,
                   static_cast<unsigned>(squad.slotType),
                   squad.slotIndex);
        }
    }
    return accepted;
}

/**
 * Stages queued output rows in one batch-owned arena and binds them to one transmission.
 * @param groupSessionId Group session the packet carries.
 * @param transmissionId Identity the outcome callback names later.
 * @param writer Packet writer positioned at the lane.
 * @return True when the lane is written, empty or not.
 */
bool write_lane0(std::uint64_t groupSessionId,
                 std::uint64_t transmissionId,
                 middleware::encoding::bits::Writer& writer) noexcept {
    if (groupSessionId == 0 || transmissionId == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_session(groupSessionId);
    if (session == nullptr || !session->policyActive) {
        ReleaseSRWLockExclusive(&g_lock);
        external::ActorCommandCatalog published{};
        if (!external::published_actor_command_catalog(published)) {
            return false;
        }
        const external::RuntimeEventPayloadCodecContext context{&published};
        const external::SimulationEventPayloadCodec codec =
            external::make_runtime_event_payload_codec(context);
        return external::write_simulation_event_lane(writer, codec, {});
    }
    if (session->catalog != state::activity_sdk::snapshot()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    external::ActorCommandCatalog catalog{};
    if (!external::published_actor_command_catalog(session->catalog, catalog)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    external::SimulationEventBatch batch{};
    std::array<OutputRow*, external::kSimulationEventCapacity> selected{};
    for (OutputRow& row : session->outputs) {
        if (row.state != OutputState::queued) {
            continue;
        }
        if (batch.count == batch.records.size()) {
            break;
        }
        external::SimulationEventRecord& record = batch.records[batch.count];
        record.eventType = static_cast<std::uint8_t>(row.draft.identity.eventType);
        record.primaryPresent = row.draft.primaryPresent;
        if ((record.primaryPresent
             && !external::append_runtime_event_body(batch, row.draft.primary, record.primary))
            || !external::append_runtime_event_body(batch, row.draft.secondary, record.secondary)) {
            ReleaseSRWLockExclusive(&g_lock);
            return false;
        }
        selected[batch.count] = &row;
        ++batch.count;
    }
    const external::RuntimeEventPayloadCodecContext context{&catalog};
    const external::SimulationEventPayloadCodec codec =
        external::make_runtime_event_payload_codec(context);
    if (!external::write_simulation_event_lane(writer, codec, batch)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    // The rows only leave the ledger once the packet owns them.
    for (std::size_t index = 0; index < batch.count; ++index) {
        selected[index]->state = OutputState::inFlight;
        selected[index]->transmissionId = transmissionId;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/**
 * Commits or retries the exact lane contribution named by one ACK outcome.
 * @param groupSessionId Group session the packet carried.
 * @param transmissionId Identity write_lane0 bound to the rows.
 * @param outcome Whether the peer acknowledged that packet.
 */
void lane0_outcome(std::uint64_t groupSessionId,
                   std::uint64_t transmissionId,
                   middleware::gameplay::peer::AckOutcome outcome) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_session(groupSessionId);
    if (session == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const bool committed = outcome == middleware::gameplay::peer::AckOutcome::received;
    for (OutputRow& row : session->outputs) {
        if (row.state != OutputState::inFlight || row.transmissionId != transmissionId) {
            continue;
        }
        if (committed) {
            // The restore barrier starts only once the client holds the command.
            if (row.purpose == OutputPurpose::restoreCommand
                && row.replayIndex < session->replays.size()
                && session->replays[row.replayIndex].occupied) {
                if (!external::mark_restore_queued(session->replays[row.replayIndex].transaction,
                                                   g_serviceFrame)) {
                    session->replays[row.replayIndex] = {};
                }
            }
            row = {};
        } else if (++row.attempts < kMaximumAttempts) {
            row.state = OutputState::queued;
            row.transmissionId = 0;
        } else {
            if (row.replayIndex < session->replays.size()) {
                session->replays[row.replayIndex] = {};
            }
            row = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::gameplay::actor_command_policy
