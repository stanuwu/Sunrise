#include "participant_credit_ledger.h"

namespace sunrise::server::gameplay::physics::mechanics {

namespace {

[[nodiscard]] std::uint64_t
saturating_add(std::uint64_t left, std::uint64_t right, bool& saturated) noexcept {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (right > maximum - left) {
        saturated = true;
        return maximum;
    }
    saturated = false;
    return left + right;
}

} // namespace

/** @return True when participant identity and channel order are valid. */
bool ParticipantCreditLedger::valid_definition(
    const ParticipantCreditDefinition& definition) noexcept {
    if (definition.participantId == 0 || definition.policyOwnerId == kAbsentPolicyOwnerId
        || definition.channelCount > definition.channelIds.size()) {
        return false;
    }
    for (std::size_t index = 0; index < definition.channelCount; ++index) {
        if (definition.channelIds[index] == 0
            || (index != 0 && definition.channelIds[index] <= definition.channelIds[index - 1])) {
            return false;
        }
    }
    return true;
}

void ParticipantCreditLedger::reset(WorldEpoch worldEpoch) noexcept {
    for (ParticipantCreditSnapshot& participant : participants_) {
        const std::uint64_t generation = next_generation(participant.generation);
        const std::uint64_t exhausted = participant.generation;
        participant = {};
        participant.generation = generation == kAbsentGeneration ? exhausted : generation;
    }
    commands_.clear();
    worldEpoch_ = worldEpoch;
}

/** Creates one policy-owned participant and explicit channel set. */
ParticipantCreditResult
ParticipantCreditLedger::create_participant(const ParticipantCreditDefinition& definition,
                                            ParticipantCreditHandle& handle) noexcept {
    handle = {};
    if (worldEpoch_ == kAbsentWorldEpoch || !valid_definition(definition)) {
        return ParticipantCreditResult::invalidDefinition;
    }
    std::size_t freeIndex = participants_.size();
    for (std::size_t index = 0; index < participants_.size(); ++index) {
        const ParticipantCreditSnapshot& participant = participants_[index];
        if (participant.active
            && participant.definition.participantId == definition.participantId) {
            return ParticipantCreditResult::invalidDefinition;
        }
        if (!participant.active && freeIndex == participants_.size()
            && next_generation(participant.generation) != kAbsentGeneration) {
            freeIndex = index;
        }
    }
    if (freeIndex == participants_.size()) {
        return ParticipantCreditResult::capacity;
    }
    ParticipantCreditSnapshot& participant = participants_[freeIndex];
    const std::uint64_t generation = next_generation(participant.generation);
    participant = {};
    participant.definition = definition;
    participant.generation = generation;
    participant.revision = 1;
    participant.active = true;
    for (std::size_t index = 0; index < definition.channelCount; ++index) {
        participant.channels[index].channelId = definition.channelIds[index];
    }
    handle = ParticipantCreditHandle{participant.generation, static_cast<std::uint16_t>(freeIndex)};
    return ParticipantCreditResult::applied;
}

ParticipantCreditResult
ParticipantCreditLedger::remove_participant(ParticipantCreditHandle handle) noexcept {
    if (!valid_handle(handle)) {
        return ParticipantCreditResult::invalidHandle;
    }
    participants_[handle.slot].active = false;
    return ParticipantCreditResult::applied;
}

/** Records one increasing eligible presence tick. */
ParticipantCreditResult
ParticipantCreditLedger::record_presence(const CommandHeader& command,
                                         ParticipantCreditHandle handle,
                                         ParticipantCreditEvent& event) noexcept {
    event = {};
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return ParticipantCreditResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_)) {
        return ParticipantCreditResult::invalidCommand;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        return ParticipantCreditResult::duplicate;
    }
    if (!valid_handle(handle)) {
        return ParticipantCreditResult::invalidHandle;
    }
    ParticipantCreditSnapshot& participant = participants_[handle.slot];
    if (participant.definition.policyOwnerId != command.policyOwnerId) {
        return ParticipantCreditResult::invalidHandle;
    }
    if (participant.hasPresence && command.executionTick <= participant.lastEligibleTick) {
        return ParticipantCreditResult::staleTick;
    }
    if (participant.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ParticipantCreditResult::revisionExhausted;
    }
    bool saturated = false;
    const std::uint64_t newPresence = saturating_add(participant.presenceTicks, 1, saturated);
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return ParticipantCreditResult::capacity;
    }
    const std::uint64_t oldPresence = participant.presenceTicks;
    if (!participant.hasPresence) {
        participant.firstEligibleTick = command.executionTick;
        participant.hasPresence = true;
    }
    participant.lastEligibleTick = command.executionTick;
    participant.presenceTicks = newPresence;
    ++participant.revision;
    event = ParticipantCreditEvent{handle,
                                   worldEpoch_,
                                   command.executionTick,
                                   participant.definition.participantId,
                                   participant.definition.policyOwnerId,
                                   participant.revision,
                                   oldPresence,
                                   newPresence,
                                   0,
                                   ParticipantCreditEventKind::presence,
                                   saturated};
    return saturated ? ParticipantCreditResult::saturated : ParticipantCreditResult::applied;
}

/** Adds one idempotent contribution with unsigned saturation. */
ParticipantCreditResult
ParticipantCreditLedger::record_contribution(const CommandHeader& command,
                                             ParticipantCreditHandle handle,
                                             std::uint32_t channelId,
                                             std::uint64_t amount,
                                             ParticipantCreditEvent& event) noexcept {
    event = {};
    if (worldEpoch_ == kAbsentWorldEpoch || command.worldEpoch != worldEpoch_) {
        return ParticipantCreditResult::staleWorld;
    }
    if (!valid_command_header(command, worldEpoch_)) {
        return ParticipantCreditResult::invalidCommand;
    }
    if (amount == 0) {
        return ParticipantCreditResult::invalidAmount;
    }
    if (commands_.contains(command.policyOwnerId, command.commandId)) {
        return ParticipantCreditResult::duplicate;
    }
    if (!valid_handle(handle)) {
        return ParticipantCreditResult::invalidHandle;
    }
    ParticipantCreditSnapshot& participant = participants_[handle.slot];
    if (participant.definition.policyOwnerId != command.policyOwnerId) {
        return ParticipantCreditResult::invalidHandle;
    }
    std::size_t channelIndex = participant.definition.channelCount;
    for (std::size_t index = 0; index < participant.definition.channelCount; ++index) {
        if (participant.channels[index].channelId == channelId) {
            channelIndex = index;
            break;
        }
    }
    if (channelIndex == participant.definition.channelCount) {
        return ParticipantCreditResult::invalidChannel;
    }
    ParticipantCreditChannel& channel = participant.channels[channelIndex];
    bool saturated = false;
    const std::uint64_t newValue = saturating_add(channel.value, amount, saturated);
    if (newValue != channel.value
        && participant.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ParticipantCreditResult::revisionExhausted;
    }
    if (commands_.remember(command) != DeduplicationResult::recorded) {
        return ParticipantCreditResult::capacity;
    }
    const std::uint64_t oldValue = channel.value;
    channel.value = newValue;
    if (newValue != oldValue) {
        ++participant.revision;
    }
    event = ParticipantCreditEvent{handle,
                                   worldEpoch_,
                                   command.executionTick,
                                   participant.definition.participantId,
                                   participant.definition.policyOwnerId,
                                   participant.revision,
                                   oldValue,
                                   newValue,
                                   channelId,
                                   ParticipantCreditEventKind::contribution,
                                   saturated};
    return saturated ? ParticipantCreditResult::saturated : ParticipantCreditResult::applied;
}

void ParticipantCreditLedger::expire_commands_before(TickId firstRetainedTick) noexcept {
    commands_.expire_before(firstRetainedTick);
}

} // namespace sunrise::server::gameplay::physics::mechanics
