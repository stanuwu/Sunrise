#include "actor_command_policy.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>

#include "../../middleware/gameplay/external/actor_command_runtime_codec.h"
#include "../../middleware/gameplay/external/simulation_event_runtime_codec.h"
#include "../../state/activity/runtime.h"
#include "../activity/mission/mission_script_runtime.h"
#include "../bap/runtime.h"
#include "actor_command_policy_internal.h"
#include "actor_command_policy_session.h"
#include "gameplay_advertisement.h"
#include "gameplay_log.h"
#include "group/group_host_sessions.h"

namespace sunrise::server::gameplay::actor_command_policy {

namespace activity = state::activity;
namespace external = middleware::gameplay::external;
namespace format = state::activity_sdk::format;
namespace mission = server::activity::mission;
namespace wire = middleware::bap::activity_message::wire_schema;

namespace {

/** Generic mission policy requests retained until a gameplay peer can carry them. */
constexpr std::size_t kDeferredPolicyCapacity = 32;

/** One generic mission policy request retained independently of gameplay-peer readiness. */
struct DeferredPolicyRow final {
    mission::ActorCommandPolicyRequest request{};
    bool bindingRetained{};
    bool occupied{};
};

/** What one register attempt did to the session row. */
enum class PolicyOutcome : std::uint8_t {
    /** Every new target now holds a queued command. */
    queued,
    /** The row changed but could not queue every command. */
    unavailable,
    /** Nothing changed, so the caller keeps the retain it took. */
    dropped,
};

/** One validated policy request, resolved against the published SDK and its host row. */
struct PolicyPlan final {
    group::HostSessionBinding host{};
    peer::LinkIdentity linkIdentity{};
    state::activity_sdk::Snapshot snapshot{};
    state::activity_sdk::generated_world::GeneratedWorldView worldView{};
    std::array<std::uint32_t, kSelectedActorClassCapacity> classes{};
    std::size_t classCount{};
    std::uint64_t activityClientGeneration{};
    SelectedSquad squad{};
    std::uint32_t messageIndex{format::kAbsentIndex};
    std::uint32_t actorEventIndex{format::kAbsentIndex};
    std::uint32_t damageEventIndex{format::kAbsentIndex};
    bool linkPresent{};
};

std::array<DeferredPolicyRow, kDeferredPolicyCapacity> g_deferredPolicies{};

/**
 * Resolves the only gameplay host allowed to execute one mission policy.
 * @param binding Exact Activity Host generation that owns the policy.
 * @param output Receives the elected host only on success.
 * @param sourceGeneration Receives the ActivityClient generation owning the policy.
 * @return True when the matching ActivityClient has an elected gameplay host.
 */
[[nodiscard]] bool resolve_policy_host(const activity::SessionBinding& binding,
                                       group::HostSessionBinding& output,
                                       std::uint64_t& sourceGeneration) noexcept {
    output = {};
    sourceGeneration = 0;
    server::bap::ActivityLinkView activityLink{};
    if (!server::bap::activity_link_view(binding, activityLink)
        || activityLink.activityClientGeneration == 0) {
        return false;
    }
    sourceGeneration = activityLink.activityClientGeneration;
    if (!activityLink.publicTarget) {
        // A private activity has one logical Bubble Host. A region host must never replace it.
        if (!server::gameplay::private_host_session(binding, output)
            && (activityLink.effectiveRegion < 0
                || !server::gameplay::complete_private_host_session(
                    binding, activityLink.effectiveRegion))) {
            return false;
        }
        return server::gameplay::private_host_session(binding, output);
    }
    return group::host_session_for_source_region(binding, activityLink.effectiveRegion, output);
}

/** @return True when both identities name the same link, not just the same session. */
[[nodiscard]] bool same_link(const peer::LinkIdentity& left,
                             const peer::LinkIdentity& right) noexcept {
    return left.localConnectionSequence == right.localConnectionSequence
           && left.remoteConnectionSequence == right.remoteConnectionSequence
           && left.viewGeneration == right.viewGeneration;
}

/** @return True when both requests carry the same policy for the same generation. */
[[nodiscard]] bool same_request(const mission::ActorCommandPolicyRequest& left,
                                const mission::ActorCommandPolicyRequest& right) noexcept {
    return same_binding(left.binding, right.binding) && left.sdkBuildSha256 == right.sdkBuildSha256
           && left.squadRow == right.squadRow && left.commandRow == right.commandRow
           && left.commandSelector == right.commandSelector && left.value == right.value;
}

/** @return True when `values` holds `value`. */
[[nodiscard]] bool contains(std::span<const std::uint32_t> values, std::uint32_t value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

/** @return The actor classes one row's policy selects. */
[[nodiscard]] std::span<const std::uint32_t> selected_classes(const SessionRow& session) noexcept {
    return {session.actorClasses.data(), session.actorClassCount};
}

/** @return True when the policy selects one SDK actor class. */
[[nodiscard]] bool class_selected(const SessionRow& session,
                                  std::uint32_t actorClassIndex) noexcept {
    return contains(selected_classes(session), actorClassIndex);
}

/** @return True when an exact token belongs to one selected live squad. */
[[nodiscard]] bool token_selected(const SessionRow& session,
                                  const external::EntityToken& token,
                                  std::size_t selectedCount) noexcept {
    return selected_entity_member(session, token, selectedCount);
}

/** @return The first free output row, or null when the ledger is full. */
[[nodiscard]] OutputRow* first_empty_output(SessionRow& session) noexcept {
    const auto found =
        std::find_if(session.outputs.begin(), session.outputs.end(), [](const auto& row) {
            return row.state == OutputState::empty;
        });
    return found == session.outputs.end() ? nullptr : &*found;
}

/**
 * Encodes one SDK-selected command for one exact live actor token.
 * @param catalog Pinned SDK rows the encoder reads.
 * @param session Row holding the resolved event, message and command indices.
 * @param actorClassIndex SDK actor class of the target.
 * @param target Live entity token the command names.
 * @param value Faction value the single command field carries.
 * @param output Receives the encoded draft only on success.
 * @return True only when the command encodes completely.
 */
[[nodiscard]] bool encode_command(const external::ActorCommandCatalog& catalog,
                                  const SessionRow& session,
                                  std::uint32_t actorClassIndex,
                                  const external::EntityToken& target,
                                  std::int32_t value,
                                  external::RuntimeEventDraft& output) noexcept {
    if (actorClassIndex >= catalog.profiles.size()
        || session.commandIndex >= catalog.commands.size()) {
        return false;
    }
    const format::ActorCommandDefinition& command = catalog.commands[session.commandIndex];
    const wire::RuntimeSchemaResolver resolver = external::actor_runtime_schema_resolver(catalog);
    wire::runtime::SchemaView schema{};
    wire::runtime::FieldView field{};
    if (!resolver.findSchema(resolver.context, command.payloadHandle, schema)
        || schema.fieldCount != 1
        || !resolver.readField(resolver.context, schema.firstField, field)) {
        return false;
    }
    wire::RuntimeDraftValue payload{};
    payload.schemaHandle = schema.handle;
    payload.schemaRow = schema.row;
    payload.fieldRow = field.row;
    payload.kind = wire::ValueKind::signedInteger;
    payload.signedValue = value;
    payload.present = true;
    external::ActorCommandHeader header{};
    // The header target is the absent marker; the token travels in the encoded body.
    header.target = (std::numeric_limits<std::uint16_t>::max)();
    wire::CodecStatus status = wire::CodecStatus::malformed;
    return external::encode_actor_message_event(catalog,
                                                session.actorEventIndex,
                                                session.messageIndex,
                                                session.commandIndex,
                                                target,
                                                header,
                                                std::span(&payload, 1),
                                                output,
                                                status)
           && status == wire::CodecStatus::complete;
}

/** Reports one policy registration outcome. */
void report_register(bool queued, const PolicyPlan& plan) noexcept {
    report(core::log::Level::info,
           "ev=actor_policy stage=register result=%s group=0x%016llX classes=%u link=%u",
           queued ? "queued" : "unavailable",
           static_cast<unsigned long long>(plan.host.groupSessionId),
           static_cast<unsigned>(plan.classCount),
           plan.linkPresent ? 1U : 0U);
}

/**
 * Resolves one policy request against the published SDK and its host row.
 * @param request Mission-supplied request naming one squad and one command.
 * @param output Filled only when the return is queued.
 * @return queued when the plan is complete, otherwise the status to report.
 */
[[nodiscard]] mission::ActorCommandPolicyStatus
plan_policy(const mission::ActorCommandPolicyRequest& request, PolicyPlan& output) noexcept {
    output = {};
    if (!activity::binding_matches(request.binding)) {
        return mission::ActorCommandPolicyStatus::refused;
    }
    if (!resolve_policy_host(request.binding, output.host, output.activityClientGeneration)) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    if (!same_binding(output.host.source, request.binding)) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    output.linkPresent = peer::link_identity(output.host.groupSessionId, output.linkIdentity);
    output.snapshot = state::activity_sdk::snapshot();
    const state::activity_sdk::Snapshot& snapshot = output.snapshot;
    if (snapshot == nullptr || snapshot->sdk_build_sha256().size() != request.sdkBuildSha256.size()
        || !std::equal(snapshot->sdk_build_sha256().begin(),
                       snapshot->sdk_build_sha256().end(),
                       request.sdkBuildSha256.begin())) {
        return mission::ActorCommandPolicyStatus::refused;
    }
    const auto squads = snapshot->squads();
    const auto commands = snapshot->actor_command_definitions();
    if (request.squadRow >= squads.size() || request.commandRow >= commands.size()) {
        return mission::ActorCommandPolicyStatus::refused;
    }
    const format::Squad& squad = squads[request.squadRow];
    state::activity_sdk::BoundView activityView{};
    server::bap::ActivityLinkView activityLink{};
    if (server::bap::activity_link_view(request.binding, activityLink)
        && state::activity_sdk::resolve(
               snapshot,
               {request.binding, activityLink.matchingLinks, activityLink.activityClientGeneration},
               activityView)
               == state::activity_sdk::Status::ready
        && activityView.scenarioRow == squad.scenarioIndex) {
        static_cast<void>(
            state::activity_sdk::generated_world::resolve(activityView, output.worldView));
    }
    const format::ActorCommandDefinition& command = commands[request.commandRow];
    if (!resolve_selected_squad(*snapshot, squad, output.squad)
        || command.flags != format::kActorCommandDefinitionExact
        || command.selector != request.commandSelector
        || command.effect != format::ActorCommandEffect::setFaction
        || (request.value != command.factionNone && request.value != command.factionRemoved
            && request.value != command.factionHostileToAll)) {
        return mission::ActorCommandPolicyStatus::refused;
    }
    external::ActorCommandCatalog catalog{};
    if (!external::published_actor_command_catalog(snapshot, catalog)) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    const format::ActorMessageSchema* const message =
        state::activity_sdk::actor_message_schema_by_name(*snapshot, "ACTOR_COMMAND");
    const format::SimulationEventDefinition* const actorEvent =
        state::activity_sdk::simulation_event_by_name(*snapshot, "ACTOR_MESSAGE");
    const format::SimulationEventDefinition* const damageEvent =
        state::activity_sdk::simulation_event_by_name(*snapshot, "DAMAGE");
    const auto members = state::activity_sdk::squad_members(*snapshot, squad);
    if (message == nullptr || actorEvent == nullptr || damageEvent == nullptr || members.empty()) {
        return mission::ActorCommandPolicyStatus::refused;
    }
    for (const format::SquadMember& member : members) {
        if (member.actorClassIndex == format::kAbsentIndex
            || member.actorClassIndex >= snapshot->actor_classes().size()) {
            return mission::ActorCommandPolicyStatus::refused;
        }
        if (contains({output.classes.data(), output.classCount}, member.actorClassIndex)) {
            continue;
        }
        if (output.classCount == output.classes.size()) {
            return mission::ActorCommandPolicyStatus::refused;
        }
        output.classes[output.classCount++] = member.actorClassIndex;
    }
    output.messageIndex = static_cast<std::uint32_t>(message - catalog.messages.data());
    output.actorEventIndex = static_cast<std::uint32_t>(actorEvent - catalog.events.data());
    output.damageEventIndex = static_cast<std::uint32_t>(damageEvent - catalog.events.data());
    return mission::ActorCommandPolicyStatus::queued;
}

/** @return True when one live row already carries exactly this policy on this link. */
[[nodiscard]] bool same_policy(const SessionRow& session,
                               const mission::ActorCommandPolicyRequest& request,
                               const PolicyPlan& plan) noexcept {
    return session.policyActive && session.bindingRetained
           && same_binding(session.binding, request.binding) && session.catalog == plan.snapshot
           && session.worldView.generation_identity() == plan.worldView.generation_identity()
           && session.activityClientGeneration == plan.activityClientGeneration
           && session.commandIndex == request.commandRow && session.policyValue == request.value
           && session.linkIdentityRetained == plan.linkPresent
           && (!plan.linkPresent || same_link(session.linkIdentity, plan.linkIdentity));
}

/**
 * Adds the classes one live policy does not hold yet and commands their live actors.
 * The caller holds the lock. A partial queue rolls the whole extension back.
 * @param session Row already carrying this exact policy.
 * @param plan Validated request naming the full class set.
 * @return dropped when nothing changed, otherwise the outcome to report.
 */
[[nodiscard]] PolicyOutcome extend_policy(SessionRow& session, const PolicyPlan& plan) noexcept {
    const std::uint8_t priorActorClassCount = session.actorClassCount;
    const std::uint8_t priorSquadCount = session.selectedSquadCount;
    const bool alreadySelected =
        std::any_of(session.selectedSquads.begin(),
                    session.selectedSquads.begin() + session.selectedSquadCount,
                    [&plan](const SelectedSquad& selected) {
                        return selected.registryKey == plan.squad.registryKey
                               && selected.slotType == plan.squad.slotType
                               && selected.slotIndex == plan.squad.slotIndex;
                    });
    if (alreadySelected) {
        return PolicyOutcome::dropped;
    }
    if (session.selectedSquadCount == session.selectedSquads.size()) {
        return PolicyOutcome::dropped;
    }
    session.selectedSquads[session.selectedSquadCount++] = plan.squad;
    for (std::size_t index = 0; index < plan.classCount; ++index) {
        if (class_selected(session, plan.classes[index])) {
            continue;
        }
        if (session.actorClassCount >= session.actorClasses.size()) {
            return PolicyOutcome::dropped;
        }
        session.actorClasses[session.actorClassCount++] = plan.classes[index];
    }
    std::size_t available = 0;
    for (const OutputRow& row : session.outputs) {
        available += row.state == OutputState::empty ? 1U : 0U;
    }
    std::size_t needed = 0;
    for (std::size_t index = 0; index < session.actors.slots.size(); ++index) {
        const external::ActorEntitySlot& actor = session.actors.slots[index];
        const external::EntityToken token{static_cast<std::uint16_t>(index), actor.incarnation};
        if (actor.occupied && class_selected(session, actor.actorClassIndex)
            && token_selected(session, token, session.selectedSquadCount)
            && !token_selected(session, token, priorSquadCount)) {
            ++needed;
        }
    }
    // Queue the whole extension or none of it, so no actor is left on the old faction.
    if (needed > available) {
        session.actorClassCount = priorActorClassCount;
        session.selectedSquadCount = priorSquadCount;
        return PolicyOutcome::dropped;
    }
    std::array<external::EntityToken, kOutputCapacity> queuedTargets{};
    std::size_t queuedTargetCount = 0;
    bool queued = true;
    for (std::size_t slotIndex = 0; slotIndex < session.actors.slots.size(); ++slotIndex) {
        const external::ActorEntitySlot& actor = session.actors.slots[slotIndex];
        const external::EntityToken token{static_cast<std::uint16_t>(slotIndex), actor.incarnation};
        if (!actor.occupied || !class_selected(session, actor.actorClassIndex)
            || !token_selected(session, token, session.selectedSquadCount)
            || token_selected(session, token, priorSquadCount)) {
            continue;
        }
        if (!queue_command(session,
                           actor.actorClassIndex,
                           token,
                           session.policyValue,
                           OutputPurpose::policyCommand)) {
            queued = false;
            break;
        }
        queuedTargets[queuedTargetCount++] = token;
    }
    if (!queued) {
        const auto rolledBack = [&queuedTargets, queuedTargetCount](const OutputRow& row) {
            return std::any_of(
                queuedTargets.begin(),
                queuedTargets.begin() + static_cast<std::ptrdiff_t>(queuedTargetCount),
                [&row](const auto& target) { return same_token(row.target, target); });
        };
        for (OutputRow& row : session.outputs) {
            if (row.purpose == OutputPurpose::policyCommand && rolledBack(row)) {
                row = {};
            }
        }
        session.actorClassCount = priorActorClassCount;
        session.selectedSquadCount = priorSquadCount;
    }
    return queued ? PolicyOutcome::queued : PolicyOutcome::unavailable;
}

/**
 * Replaces one row's policy and commands every live actor it selects.
 * The caller holds the lock and transfers its activity retain into the row.
 * @param session Row to overwrite.
 * @param request Mission-supplied request the row adopts.
 * @param plan Validated resolution of that request.
 * @return dropped when nothing changed, otherwise the outcome to report.
 */
[[nodiscard]] PolicyOutcome apply_policy(SessionRow& session,
                                         const mission::ActorCommandPolicyRequest& request,
                                         const PolicyPlan& plan) noexcept {
    const std::span<const std::uint32_t> planClasses{plan.classes.data(), plan.classCount};
    // A new link means a new entity view, so the old tokens name nothing and are dropped.
    const bool linkChanged = plan.linkPresent && session.linkIdentityRetained
                             && !same_link(session.linkIdentity, plan.linkIdentity);
    if (!linkChanged) {
        std::size_t existingTargets = 0;
        for (std::size_t index = 0; index < session.actors.slots.size(); ++index) {
            const external::ActorEntitySlot& actor = session.actors.slots[index];
            const external::EntityToken token{static_cast<std::uint16_t>(index), actor.incarnation};
            if (actor.occupied && contains(planClasses, actor.actorClassIndex)
                && token_selected(session, token, session.selectedSquadCount)) {
                ++existingTargets;
            }
        }
        if (existingTargets > session.outputs.size()) {
            return PolicyOutcome::dropped;
        }
    }
    session.binding = request.binding;
    session.catalog = plan.snapshot;
    session.worldView = plan.worldView;
    session.groupSessionId = plan.host.groupSessionId;
    session.hostGeneration = plan.host.generation;
    session.activityClientGeneration = plan.activityClientGeneration;
    session.linkIdentity = plan.linkIdentity;
    session.linkIdentityRetained = plan.linkPresent;
    session.actorClasses = plan.classes;
    session.actorClassCount = static_cast<std::uint8_t>(plan.classCount);
    session.selectedSquads = {};
    session.selectedSquads[0] = plan.squad;
    session.selectedSquadCount = 1;
    session.messageIndex = plan.messageIndex;
    session.actorEventIndex = plan.actorEventIndex;
    session.damageEventIndex = plan.damageEventIndex;
    session.commandIndex = request.commandRow;
    session.policyValue = request.value;
    if (linkChanged) {
        external::reset_actor_entity_registry(session.actors);
        session.squads = {};
    }
    session.outputs = {};
    session.replays = {};
    session.bindingRetained = true;
    session.policyActive = true;
    bool queued = true;
    for (std::size_t slotIndex = 0; slotIndex < session.actors.slots.size(); ++slotIndex) {
        const external::ActorEntitySlot& actor = session.actors.slots[slotIndex];
        const external::EntityToken token{static_cast<std::uint16_t>(slotIndex), actor.incarnation};
        if (!actor.occupied || !class_selected(session, actor.actorClassIndex)
            || !token_selected(session, token, session.selectedSquadCount)) {
            continue;
        }
        queued = queue_command(session,
                               actor.actorClassIndex,
                               token,
                               session.policyValue,
                               OutputPurpose::policyCommand)
                 && queued;
    }
    return queued ? PolicyOutcome::queued : PolicyOutcome::unavailable;
}

/**
 * Installs or extends one durable group policy.
 * @param request Mission-supplied request naming one squad and one command.
 * @return queued once every live target holds a command.
 */
[[nodiscard]] mission::ActorCommandPolicyStatus
install_policy(const mission::ActorCommandPolicyRequest& request) noexcept {
    PolicyPlan plan{};
    const mission::ActorCommandPolicyStatus planned = plan_policy(request, plan);
    if (planned != mission::ActorCommandPolicyStatus::queued) {
        return planned;
    }
    if (!activity::retain_binding(request.binding)) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    bool extended = false;
    bool releasePrior = false;
    activity::SessionBinding prior{};
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_or_create_session(plan.host.groupSessionId);
    PolicyOutcome outcome = PolicyOutcome::dropped;
    if (session != nullptr) {
        extended = same_policy(*session, request, plan);
        if (extended) {
            outcome = extend_policy(*session, plan);
        } else {
            releasePrior = session->bindingRetained;
            prior = session->binding;
            outcome = apply_policy(*session, request, plan);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (extended || outcome == PolicyOutcome::dropped) {
        activity::release_binding(request.binding);
    } else {
        // A partial install leaves no usable policy, so the row and its retain go together.
        if (outcome != PolicyOutcome::queued) {
            reset_group_session(plan.host.groupSessionId);
        }
        if (releasePrior) {
            activity::release_binding(prior);
        }
    }
    if (outcome == PolicyOutcome::dropped) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    report_register(outcome == PolicyOutcome::queued, plan);
    return outcome == PolicyOutcome::queued ? mission::ActorCommandPolicyStatus::queued
                                            : mission::ActorCommandPolicyStatus::unavailable;
}

/**
 * Answers one mission policy request, retaining it when no peer can carry it yet.
 * @param request Mission-supplied request naming one squad and one command.
 * @return queued once the policy is installed or retained for a later service slice.
 */
[[nodiscard]] mission::ActorCommandPolicyStatus
policy_callback(const void*, const mission::ActorCommandPolicyRequest& request) noexcept {
    const mission::ActorCommandPolicyStatus status = install_policy(request);
    if (status != mission::ActorCommandPolicyStatus::unavailable
        || !activity::retain_binding(request.binding)) {
        return status;
    }
    bool stored = false;
    bool duplicate = false;
    AcquireSRWLockExclusive(&g_lock);
    const auto existing = std::find_if(
        g_deferredPolicies.begin(), g_deferredPolicies.end(), [&request](const auto& row) {
            return row.occupied && same_request(row.request, request);
        });
    if (existing != g_deferredPolicies.end()) {
        duplicate = true;
    } else {
        const auto empty = std::find_if(g_deferredPolicies.begin(),
                                        g_deferredPolicies.end(),
                                        [](const auto& row) { return !row.occupied; });
        if (empty != g_deferredPolicies.end()) {
            empty->request = request;
            empty->bindingRetained = true;
            empty->occupied = true;
            stored = true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    // Only a stored row owns a retain. A duplicate already has one.
    if (!stored) {
        activity::release_binding(request.binding);
    }
    if (!stored && !duplicate) {
        return mission::ActorCommandPolicyStatus::unavailable;
    }
    report(core::log::Level::info,
           "ev=actor_policy stage=defer result=queued session=0x%016llX squad=%u",
           static_cast<unsigned long long>(request.binding.sessionId),
           static_cast<unsigned>(request.squadRow));
    return mission::ActorCommandPolicyStatus::queued;
}

/** Retries every retained request and clears the rows that no longer need one. */
void service_deferred_policies() noexcept {
    for (std::size_t index = 0; index < g_deferredPolicies.size(); ++index) {
        mission::ActorCommandPolicyRequest request{};
        AcquireSRWLockShared(&g_lock);
        const bool occupied = g_deferredPolicies[index].occupied;
        if (occupied) {
            request = g_deferredPolicies[index].request;
        }
        ReleaseSRWLockShared(&g_lock);
        if (!occupied
            || install_policy(request) == mission::ActorCommandPolicyStatus::unavailable) {
            continue;
        }
        bool release = false;
        AcquireSRWLockExclusive(&g_lock);
        DeferredPolicyRow& current = g_deferredPolicies[index];
        if (current.occupied && same_request(current.request, request)) {
            release = current.bindingRetained;
            current = {};
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (release) {
            activity::release_binding(request.binding);
        }
    }
}

/** Drops every transport-scoped field and adopts one link identity. The caller holds the lock. */
void adopt_link_identity(SessionRow& session,
                         const peer::LinkIdentity& linkIdentity,
                         bool present) noexcept {
    external::reset_actor_entity_registry(session.actors);
    session.squads = {};
    session.outputs = {};
    session.replays = {};
    session.linkIdentity = present ? linkIdentity : peer::LinkIdentity{};
    session.linkIdentityRetained = present;
}

/** Moves every replay past its barrier into the output ledger. The caller holds the lock. */
void service_replays(SessionRow& session, const external::ActorCommandCatalog& catalog) noexcept {
    for (ReplayRow& replay : session.replays) {
        if (!replay.occupied
            || !external::advance_event_replay(replay.transaction, g_serviceFrame)) {
            continue;
        }
        OutputRow* const output = first_empty_output(session);
        if (output == nullptr) {
            continue;
        }
        external::RuntimeEventDraft draft{};
        wire::CodecStatus status = wire::CodecStatus::malformed;
        if (!external::take_event_replay(replay.transaction, catalog, draft, status)) {
            continue;
        }
        output->draft = draft;
        output->target = replay.target;
        output->state = OutputState::queued;
        output->purpose = OutputPurpose::replay;
        replay = {};
    }
}

} // namespace

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<SessionRow, kSessionCapacity> g_sessions{};
std::uint64_t g_serviceFrame{};

/** Reconstructs the large fixed row directly in its owned storage. */
static void clear_session_row(SessionRow& row) noexcept {
    std::destroy_at(&row);
    std::construct_at(&row);
}

SessionRow* find_session(std::uint64_t groupSessionId) noexcept {
    const auto found =
        std::find_if(g_sessions.begin(), g_sessions.end(), [groupSessionId](auto& row) {
            return row.occupied && row.groupSessionId == groupSessionId;
        });
    return found == g_sessions.end() ? nullptr : &*found;
}

/** @return The row for one group session, claiming a free row when it has none. */
SessionRow* find_or_create_session(std::uint64_t groupSessionId) noexcept {
    if (groupSessionId == 0) {
        return nullptr;
    }
    if (SessionRow* const found = find_session(groupSessionId); found != nullptr) {
        return found;
    }
    const auto empty = std::find_if(
        g_sessions.begin(), g_sessions.end(), [](const auto& row) { return !row.occupied; });
    if (empty == g_sessions.end()) {
        return nullptr;
    }
    empty->groupSessionId = groupSessionId;
    empty->occupied = true;
    return &*empty;
}

/** Queues one command for the session. @return True when a queue slot was free. */
bool queue_command(SessionRow& session,
                   std::uint32_t actorClassIndex,
                   const external::EntityToken& target,
                   std::int32_t value,
                   OutputPurpose purpose,
                   std::uint32_t replayIndex) noexcept {
    external::ActorCommandCatalog catalog{};
    OutputRow* const row = first_empty_output(session);
    if (row == nullptr || !external::published_actor_command_catalog(session.catalog, catalog)) {
        return false;
    }
    if (purpose == OutputPurpose::policyCommand
        && !resolve_policy_faction(session, target, actorClassIndex, catalog.profiles, value)) {
        return false;
    }
    OutputRow candidate{};
    if (!encode_command(catalog, session, actorClassIndex, target, value, candidate.draft)) {
        return false;
    }
    candidate.target = target;
    candidate.replayIndex = replayIndex;
    candidate.state = OutputState::queued;
    candidate.purpose = purpose;
    *row = candidate;
    return true;
}

/** Clears transient state but retains policy. */
void internal::reset_transport_session(std::uint64_t groupSessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_session(groupSessionId);
    if (session != nullptr) {
        adopt_link_identity(*session, {}, false);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void initialize() noexcept {
    shutdown();
    mission::install_actor_command_policy(nullptr, &policy_callback);
}

/** Uninstalls transport callbacks and releases every retained activity binding. */
void shutdown() noexcept {
    mission::install_actor_command_policy(nullptr, nullptr);
    peer::install_lane0_transport({});
    internal::shutdown_entity_transport();
    std::array<activity::SessionBinding, kSessionCapacity + kDeferredPolicyCapacity> releases{};
    std::size_t releaseCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (SessionRow& row : g_sessions) {
        if (row.bindingRetained) {
            releases[releaseCount++] = row.binding;
        }
        clear_session_row(row);
    }
    for (DeferredPolicyRow& row : g_deferredPolicies) {
        if (row.bindingRetained) {
            releases[releaseCount++] = row.request.binding;
        }
        row = {};
    }
    g_serviceFrame = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < releaseCount; ++index) {
        activity::release_binding(releases[index]);
    }
}

/** Validates sessions and advances replay barriers. */
void service(std::uint64_t) noexcept {
    if (!internal::entity_transport_ready()) {
        static_cast<void>(install_entity_transport());
    }
    service_deferred_policies();
    std::array<activity::SessionBinding, kSessionCapacity> bindings{};
    std::array<std::uint64_t, kSessionCapacity> groups{};
    std::array<std::uint64_t, kSessionCapacity> generations{};
    std::array<peer::LinkIdentity, kSessionCapacity> linkIdentities{};
    std::array<bool, kSessionCapacity> linkPresent{};
    std::array<state::activity_sdk::Snapshot, kSessionCapacity> catalogs{};
    std::size_t count = 0;
    AcquireSRWLockShared(&g_lock);
    for (const SessionRow& row : g_sessions) {
        if (row.occupied && row.policyActive) {
            bindings[count] = row.binding;
            groups[count] = row.groupSessionId;
            generations[count] = row.hostGeneration;
            catalogs[count] = row.catalog;
            ++count;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    // The activity, host and link lookups take their own locks, so they run unlocked here.
    std::array<bool, kSessionCapacity> valid{};
    for (std::size_t index = 0; index < count; ++index) {
        group::HostSessionBinding host{};
        valid[index] = catalogs[index] == state::activity_sdk::snapshot()
                       && activity::binding_matches(bindings[index])
                       && group::host_session_for_group(groups[index], host)
                       && host.generation == generations[index]
                       && same_binding(host.source, bindings[index]);
        linkPresent[index] = peer::link_identity(groups[index], linkIdentities[index]);
    }
    std::array<activity::SessionBinding, kSessionCapacity> releases{};
    std::size_t releaseCount = 0;
    AcquireSRWLockExclusive(&g_lock);
    ++g_serviceFrame;
    for (std::size_t index = 0; index < count; ++index) {
        SessionRow* const session = find_session(groups[index]);
        if (session == nullptr || session->hostGeneration != generations[index]) {
            continue;
        }
        if (!valid[index]) {
            if (session->bindingRetained) {
                releases[releaseCount++] = session->binding;
            }
            clear_session_row(*session);
            continue;
        }
        const bool linkMoved = linkPresent[index]
                               && (!session->linkIdentityRetained
                                   || !same_link(session->linkIdentity, linkIdentities[index]));
        if ((!linkPresent[index] && session->linkIdentityRetained) || linkMoved) {
            adopt_link_identity(*session, linkIdentities[index], linkPresent[index]);
        }
        external::ActorCommandCatalog catalog{};
        if (!external::published_actor_command_catalog(session->catalog, catalog)) {
            continue;
        }
        service_replays(*session, catalog);
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < releaseCount; ++index) {
        activity::release_binding(releases[index]);
    }
}

/** Clears all policy, actor and replay state owned by one replaced group session. */
void reset_group_session(std::uint64_t groupSessionId) noexcept {
    activity::SessionBinding release{};
    bool retained = false;
    AcquireSRWLockExclusive(&g_lock);
    SessionRow* const session = find_session(groupSessionId);
    if (session != nullptr) {
        release = session->binding;
        retained = session->bindingRetained;
        clear_session_row(*session);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (retained) {
        activity::release_binding(release);
    }
}

} // namespace sunrise::server::gameplay::actor_command_policy
