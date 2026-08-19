#include "participant_credit_ledger.h"

namespace sunrise::server::gameplay::physics::mechanics {

bool ParticipantCreditLedger::query(ParticipantCreditHandle handle,
                                    ParticipantCreditSnapshot& participant) const noexcept {
    if (!valid_handle(handle)) {
        return false;
    }
    participant = participants_[handle.slot];
    return true;
}

ParticipantCreditLedgerSnapshot ParticipantCreditLedger::snapshot() const noexcept {
    return ParticipantCreditLedgerSnapshot{participants_, commands_.snapshot(), worldEpoch_};
}

/** Replaces participant credit only after every retained channel validates. */
bool ParticipantCreditLedger::restore(const ParticipantCreditLedgerSnapshot& snapshot) noexcept {
    if (snapshot.worldEpoch == kAbsentWorldEpoch) {
        return false;
    }
    for (std::size_t left = 0; left < snapshot.participants.size(); ++left) {
        const ParticipantCreditSnapshot& participant = snapshot.participants[left];
        if (participant.generation == kAbsentGeneration) {
            return false;
        }
        if (!participant.active) {
            continue;
        }
        if (!valid_definition(participant.definition) || participant.revision == 0
            || (participant.hasPresence
                && (participant.presenceTicks == 0
                    || participant.firstEligibleTick > participant.lastEligibleTick))
            || (!participant.hasPresence
                && (participant.presenceTicks != 0 || participant.firstEligibleTick != 0
                    || participant.lastEligibleTick != 0))) {
            return false;
        }
        for (std::size_t index = 0; index < participant.definition.channelCount; ++index) {
            if (participant.channels[index].channelId != participant.definition.channelIds[index]) {
                return false;
            }
        }
        for (std::size_t index = participant.definition.channelCount;
             index < participant.channels.size();
             ++index) {
            if (participant.channels[index].channelId != 0
                || participant.channels[index].value != 0) {
                return false;
            }
        }
        for (std::size_t right = left + 1; right < snapshot.participants.size(); ++right) {
            const ParticipantCreditSnapshot& other = snapshot.participants[right];
            if (other.active
                && participant.definition.participantId == other.definition.participantId) {
                return false;
            }
        }
    }
    CommandDeduplicator restoredCommands{};
    if (!restoredCommands.restore(snapshot.commands)) {
        return false;
    }
    participants_ = snapshot.participants;
    commands_ = restoredCommands;
    worldEpoch_ = snapshot.worldEpoch;
    return true;
}

bool ParticipantCreditLedger::valid_handle(ParticipantCreditHandle handle) const noexcept {
    return handle.slot < participants_.size() && handle.generation != kAbsentGeneration
           && participants_[handle.slot].active
           && participants_[handle.slot].generation == handle.generation;
}

} // namespace sunrise::server::gameplay::physics::mechanics
