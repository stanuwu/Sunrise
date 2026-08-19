#include "activity_policy.h"

#include <limits>

namespace sunrise::server::gameplay::physics::world {
namespace {

/** The fallback policy uses one stable authored host identifier and schema version. */
constexpr BlueprintRef kDefaultPolicyBlueprint{0x44454641554C5401ULL, 1};
/** The empty fallback state starts with persistence schema one. */
constexpr std::uint32_t kDefaultPolicySchemaVersion = 1;

} // namespace

HostCommands::HostCommands(CommandQueue& queue,
                           const ActivityPolicyManifest& manifest,
                           WorldIdentity world,
                           CommandSource source,
                           TickId minimumTick,
                           std::uint64_t& sourceSequence) noexcept
    : queue_(&queue), manifest_(&manifest), world_(world), source_(source),
      minimumTick_(minimumTick), sourceSequence_(&sourceSequence) {}

/**
 * Stamps one policy request and enforces its manifest before queue admission.
 * @param meta Deterministic policy-owned command identity.
 * @param payload Typed host operation.
 * @return Queue or policy rejection status.
 */
CommandSubmitStatus HostCommands::submit(const PolicyCommandMeta& meta,
                                         HostCommandPayload payload) noexcept {
    if (queue_ == nullptr || manifest_ == nullptr || sourceSequence_ == nullptr
        || minimumTick_ == std::numeric_limits<TickId>::max() || meta.executeTick < minimumTick_
        || meta.commandId == 0 || meta.policyOwnerId == 0 || !valid_blueprint(meta.definition)
        || *sourceSequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return meta.executeTick < minimumTick_ ? CommandSubmitStatus::late
                                               : CommandSubmitStatus::invalid;
    }

    HostCommand command{};
    command.header.world = world_;
    command.header.definition = meta.definition;
    command.header.source = source_;
    command.header.commandId = meta.commandId;
    command.header.executeTick = meta.executeTick;
    command.header.policyOwnerId = meta.policyOwnerId;
    command.header.sourceSequence = ++(*sourceSequence_);
    command.payload = payload;
    if ((manifest_->allowedCommandMask & command_kind_mask(command_kind(command))) == 0) {
        return CommandSubmitStatus::policyDenied;
    }
    return queue_->push(command);
}

CommandSubmitStatus HostCommands::spawn_actor(const PolicyCommandMeta& meta,
                                              const SpawnActorCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::remove_actor(const PolicyCommandMeta& meta,
                                               const RemoveActorCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::set_motion_authority(const PolicyCommandMeta& meta,
                                   const SetMotionAuthorityCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::set_kinematic_target(const PolicyCommandMeta& meta,
                                   const SetKinematicTargetCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::teleport_actor(const PolicyCommandMeta& meta,
                                                 const TeleportActorCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::configure_controller(const PolicyCommandMeta& meta,
                                   const ConfigureControllerCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::request_path(const PolicyCommandMeta& meta,
                                               const RequestPathCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::configure_combat(const PolicyCommandMeta& meta,
                                                   const ConfigureCombatCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::submit_damage(const PolicyCommandMeta& meta,
                                                const SubmitDamageCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::create_trigger(const PolicyCommandMeta& meta,
                                                 const CreateTriggerCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus HostCommands::remove_trigger(const PolicyCommandMeta& meta,
                                                 const RemoveTriggerCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::add_objective_counter(const PolicyCommandMeta& meta,
                                    const AddObjectiveCounterCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::set_objective_counter(const PolicyCommandMeta& meta,
                                    const SetObjectiveCounterCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::record_participant_credit(const PolicyCommandMeta& meta,
                                        const RecordParticipantCreditCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::emit_mapped_incident(const PolicyCommandMeta& meta,
                                   const EmitMappedIncidentCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::schedule_tick_timer(const PolicyCommandMeta& meta,
                                  const ScheduleTickTimerCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::request_checkpoint(const PolicyCommandMeta& meta,
                                 const RequestCheckpointCommand& command) noexcept {
    return submit(meta, command);
}

CommandSubmitStatus
HostCommands::submit_reward_intent(const PolicyCommandMeta& meta,
                                   const SubmitRewardIntentCommand& command) noexcept {
    return submit(meta, command);
}

ActivityPolicyManifest DefaultActivityPolicy::manifest() const noexcept {
    ActivityPolicyManifest result{};
    result.policy = kDefaultPolicyBlueprint;
    result.fixedRateHz = kDefaultFixedRateHz;
    result.persistenceSchemaVersion = kDefaultPolicySchemaVersion;
    return result;
}

bool DefaultActivityPolicy::initialize(const ActivityPolicyContext& context,
                                       HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(commands);
    return true;
}

void DefaultActivityPolicy::pre_tick(const PolicyTickContext& context,
                                     HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(commands);
}

void DefaultActivityPolicy::post_tick(const PolicyTickContext& context,
                                      std::span<const CommittedEvent> committedEvents,
                                      HostCommands& commands) noexcept {
    static_cast<void>(context);
    static_cast<void>(committedEvents);
    static_cast<void>(commands);
}

bool DefaultActivityPolicy::save(IPolicyStateWriter& writer) const noexcept {
    static_cast<void>(writer);
    return true;
}

bool DefaultActivityPolicy::load(IPolicyStateReader& reader) noexcept {
    static_cast<void>(reader);
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
