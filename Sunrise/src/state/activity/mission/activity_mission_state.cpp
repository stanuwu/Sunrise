#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include "../../runtime/storage/internal.h"
#include "../transactions/internal.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::state::activity::mission {

/** True when every hash, tag and index of a program key is set. */
[[nodiscard]] bool valid_program(const ProgramKey& program) noexcept {
    const bool hasBuild = std::any_of(program.sdkBuildSha256.begin(),
                                      program.sdkBuildSha256.end(),
                                      [](std::byte value) { return value != std::byte{}; });
    const bool hasSource = std::any_of(program.scriptSourceSha256.begin(),
                                       program.scriptSourceSha256.end(),
                                       [](std::byte value) { return value != std::byte{}; });
    const bool hasWorld = std::any_of(program.worldGenerationSha256.begin(),
                                      program.worldGenerationSha256.end(),
                                      [](std::byte value) { return value != std::byte{}; });
    return hasBuild && hasWorld && hasSource && program.activityDefinition != 0
           && program.worldScenarioTag != 0 && program.activityIndex >= 0;
}

[[nodiscard]] bool same_program(const ProgramKey& left, const ProgramKey& right) noexcept {
    return left.sdkBuildSha256 == right.sdkBuildSha256
           && left.worldGenerationSha256 == right.worldGenerationSha256
           && left.scriptSourceSha256 == right.scriptSourceSha256
           && left.activityDefinition == right.activityDefinition
           && left.worldScenarioTag == right.worldScenarioTag
           && left.activityIndex == right.activityIndex && left.publicTarget == right.publicTarget;
}

[[nodiscard]] SessionRecord* find_record(ActivityState& state,
                                         const SessionBinding& binding) noexcept {
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    return slot < state.sessions.size()
                   && transactions::record_matches(state.sessions[slot], binding)
               ? &state.sessions[slot]
               : nullptr;
}

[[nodiscard]] const SessionRecord* find_record(const ActivityState& state,
                                               const SessionBinding& binding) noexcept {
    const std::size_t slot = transactions::find_session(state, binding.sessionId);
    return slot < state.sessions.size()
                   && transactions::record_matches(state.sessions[slot], binding)
               ? &state.sessions[slot]
               : nullptr;
}

/** Publishes one committed record to the caller. */
[[nodiscard]] bool copy_snapshot(const ActivityState& activity,
                                 const SessionBinding&,
                                 const SessionRecord& record,
                                 Snapshot& output) noexcept {
    try {
        output.state = record.mission;
    } catch (const std::bad_alloc&) {
        output = {};
        return false;
    }
    output.activityStateRevision = activity.stateRevision;
    return true;
}

/** Copies one durable mission value without letting allocation cross a State boundary. */
[[nodiscard]] bool copy_mission_state(const MissionState& source, MissionState& output) noexcept {
    try {
        output = source;
        return true;
    } catch (const std::bad_alloc&) {
        output = {};
        return false;
    }
}

[[nodiscard]] bool publish(ActivityState& activity, SessionRecord& record) noexcept {
    if (activity.stateRevision == kMaximumRevision) {
        return false;
    }
    ++activity.stateRevision;
    record.recordRevision = activity.stateRevision;
    return true;
}

/** @return The exact head intent after common delivery compare guards pass. */
[[nodiscard]] Status checked_head(SessionRecord& record,
                                  const ProgramKey& program,
                                  std::uint64_t expectedMissionRevision,
                                  std::uint64_t expectedIntentSequence,
                                  PendingIntent*& output) noexcept {
    output = nullptr;
    if (!record.mission.programBound || !same_program(record.mission.program, program)) {
        return Status::programMismatch;
    }
    if (record.mission.revision != expectedMissionRevision) {
        return Status::revisionMismatch;
    }
    if (record.mission.pendingIntents.empty()
        || record.mission.pendingIntents[0].sequence != expectedIntentSequence) {
        return Status::intentMismatch;
    }
    output = &record.mission.pendingIntents[0];
    return Status::ready;
}

namespace {

/** Field-by-field equality of two typed intents. */
[[nodiscard]] bool same_intent(const TypedIntent& left, const TypedIntent& right) noexcept {
    return left.squadCounts == right.squadCounts && left.authBody == right.authBody
           && left.sequenceOwner == right.sequenceOwner
           && left.sdkBuildSha256 == right.sdkBuildSha256 && left.kind == right.kind
           && left.firstRow == right.firstRow && left.secondRow == right.secondRow
           && left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.sceneEventKey == right.sceneEventKey && left.authSchema == right.authSchema
           && left.actorCommandSelector == right.actorCommandSelector
           && left.actorCommandValue == right.actorCommandValue
           && left.authBitCount == right.authBitCount
           && left.effectiveRegion == right.effectiveRegion
           && left.retirePlacedProps == right.retirePlacedProps && left.slotIndex == right.slotIndex
           && left.deviceValue == right.deviceValue && left.squadMode == right.squadMode
           && left.squadRetireOnReturn == right.squadRetireOnReturn
           && left.squadCount == right.squadCount && left.deviceChannel == right.deviceChannel
           && left.slotType == right.slotType && left.authByteCount == right.authByteCount
           && left.lifetimeState == right.lifetimeState && left.deviceSnap == right.deviceSnap
           && left.active == right.active && left.requestKey == right.requestKey;
}

[[nodiscard]] std::string_view key_view(const StateKey& key) noexcept {
    return {key.bytes.data(), key.length};
}

/** @return True for one canonical bounded identifier with zero-filled trailing storage. */
[[nodiscard]] bool valid_key(const StateKey& key) noexcept {
    if (key.length == 0 || key.length >= key.bytes.size() || key.bytes[key.length] != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < key.length; ++index) {
        const unsigned char value = static_cast<unsigned char>(key.bytes[index]);
        const bool alphaNumeric = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                                  || (value >= '0' && value <= '9');
        if (!alphaNumeric && value != '_' && value != '-' && value != '.' && value != '/') {
            return false;
        }
    }
    return std::all_of(key.bytes.begin() + key.length + 1, key.bytes.end(), [](char value) {
        return value == '\0';
    });
}

/** True when only the fields of the value's kind are set and the string is terminated. */
[[nodiscard]] bool valid_variable_value(const VariableValue& value) noexcept {
    const bool clearedString = value.stringLength == 0
                               && std::all_of(value.stringValue.begin(),
                                              value.stringValue.end(),
                                              [](char item) { return item == '\0'; });
    switch (value.kind) {
    case VariableValueKind::boolean:
        return value.integerValue == 0 && value.realValue == 0.0 && clearedString;
    case VariableValueKind::integer:
        return !value.booleanValue && value.realValue == 0.0 && clearedString;
    case VariableValueKind::real:
        return !value.booleanValue && value.integerValue == 0 && std::isfinite(value.realValue)
               && clearedString;
    case VariableValueKind::string:
        return !value.booleanValue && value.integerValue == 0 && value.realValue == 0.0
               && value.stringLength < value.stringValue.size()
               && std::all_of(
                   value.stringValue.begin() + value.stringLength,
                   value.stringValue.end(),
                   [](char item) { return item == '\0'; });
    }
    return false;
}

/** True when the variables fit, each is valid and the keys are strictly ascending. */
[[nodiscard]] bool valid_variables(std::span<const ScriptVariable> variables) noexcept {
    if (variables.size() > kVariableCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < variables.size(); ++index) {
        if (!valid_key(variables[index].key) || !valid_variable_value(variables[index].value)
            || (index != 0
                && !(key_view(variables[index - 1].key) < key_view(variables[index].key)))) {
            return false;
        }
    }
    return true;
}

/** True when the timers fit, are keyed in ascending order and sit below the next sequence. */
[[nodiscard]] bool valid_timers(const MissionState& current,
                                std::span<const MissionTimer> timers,
                                std::uint64_t nextSequence) noexcept {
    if (timers.size() > kTimerCapacity
        || (current.nextTimerSequence == kAbsentTimerSequence
            && nextSequence != kAbsentTimerSequence)
        || (current.nextTimerSequence != kAbsentTimerSequence
            && nextSequence == kAbsentTimerSequence
            && current.nextTimerSequence != (std::numeric_limits<std::uint64_t>::max)())
        || (current.nextTimerSequence != kAbsentTimerSequence
            && nextSequence != kAbsentTimerSequence && nextSequence < current.nextTimerSequence)) {
        return false;
    }
    for (std::size_t index = 0; index < timers.size(); ++index) {
        const MissionTimer& timer = timers[index];
        if (!valid_key(timer.key) || timer.sequence == kAbsentTimerSequence
            || (nextSequence != kAbsentTimerSequence && timer.sequence >= nextSequence)
            || (index != 0 && !(key_view(timers[index - 1].key) < key_view(timer.key)))) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (timers[prior].sequence == timer.sequence) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool same_value(const VariableValue& left, const VariableValue& right) noexcept {
    return left.stringValue == right.stringValue && left.integerValue == right.integerValue
           && left.realValue == right.realValue && left.stringLength == right.stringLength
           && left.kind == right.kind && left.booleanValue == right.booleanValue;
}

/** True when the current variables match the span key by key and value by value. */
[[nodiscard]] bool same_variables(const MissionState& current,
                                  std::span<const ScriptVariable> variables) noexcept {
    if (current.variableCount != variables.size()) {
        return false;
    }
    for (std::size_t index = 0; index < variables.size(); ++index) {
        if (current.variables[index].key.bytes != variables[index].key.bytes
            || current.variables[index].key.length != variables[index].key.length
            || !same_value(current.variables[index].value, variables[index].value)) {
            return false;
        }
    }
    return true;
}

/** True when the current timers match the span in key, deadline and sequence. */
[[nodiscard]] bool same_timers(const MissionState& current,
                               std::span<const MissionTimer> timers) noexcept {
    if (current.timerCount != timers.size()) {
        return false;
    }
    for (std::size_t index = 0; index < timers.size(); ++index) {
        if (current.timers[index].key.bytes != timers[index].key.bytes
            || current.timers[index].key.length != timers[index].key.length
            || current.timers[index].deadlineTick != timers[index].deadlineTick
            || current.timers[index].sequence != timers[index].sequence) {
            return false;
        }
    }
    return true;
}

/** Extends the current durable queue with the VM's complete ordered outbox. */
[[nodiscard]] Status merge_intents(const MissionState& current,
                                   std::uint64_t nextMissionRevision,
                                   std::span<const TypedIntent> intents,
                                   MissionState& candidate) noexcept {
    if (intents.size() < current.pendingIntents.size()) {
        return Status::intentMismatch;
    }
    for (std::size_t index = 0; index < current.pendingIntents.size(); ++index) {
        if (!same_intent(current.pendingIntents[index].value, intents[index])) {
            return Status::intentMismatch;
        }
    }
    if (!copy_mission_state(current, candidate)) {
        return Status::outOfMemory;
    }
    try {
        candidate.pendingIntents.reserve(intents.size());
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    for (std::size_t index = current.pendingIntents.size(); index < intents.size(); ++index) {
        if (candidate.nextIntentSequence == kAbsentIntentSequence) {
            return Status::intentSequenceExhausted;
        }
        PendingIntent pending{};
        try {
            pending.value = intents[index];
        } catch (const std::bad_alloc&) {
            return Status::outOfMemory;
        }
        pending.sequence = candidate.nextIntentSequence;
        pending.missionRevision = nextMissionRevision;
        candidate.pendingIntents.push_back(std::move(pending));
        candidate.nextIntentSequence =
            candidate.nextIntentSequence == (std::numeric_limits<std::uint64_t>::max)()
                ? kAbsentIntentSequence
                : candidate.nextIntentSequence + 1;
    }
    return Status::ready;
}

/**
 * Orders the mission preconditions so the first failing one names the refusal.
 * @return Ready when the transaction may advance the stored mission.
 */
[[nodiscard]] Status transition_status(const MissionState& mission,
                                       const CommitCandidate& transaction) noexcept {
    if (transaction.nextInputSequence > mission.issuedInputSequence
        || (!transaction.started && mission.started) || mission.faulted) {
        return Status::invalidTransition;
    }
    if (!valid_timers(mission, transaction.timers, transaction.nextTimerSequence)) {
        return Status::invalidTimer;
    }
    if (transaction.nextIntentKey < mission.nextIntentKey) {
        return Status::invalidTransition;
    }
    return Status::ready;
}

} // namespace

/** Binds a program once, or reads the existing exact program state. */
Status bind(const SessionBinding& binding, const ProgramKey& program, Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (record->mission.programBound && !same_program(record->mission.program, program)) {
        status = Status::programMismatch;
    } else if (!record->mission.programBound) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else if (!publish(activity, *record)) {
            status = Status::revisionExhausted;
        } else {
            candidate.program = program;
            candidate.programBound = true;
            // Inputs accepted before a script could bind are the baseline, not replayable program
            // callbacks. Retire them atomically with the first program binding so the first input
            // issued after attachment is the next contiguous callback.
            candidate.inputSequence = candidate.issuedInputSequence;
            record->mission = std::move(candidate);
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Replaces the bound program identity while keeping the mission record. */
Status rebind_program(const SessionBinding& binding,
                      const ProgramKey& expected,
                      const ProgramKey& replacement,
                      Snapshot& output) noexcept {
    output = {};
    if (!valid_program(expected) || !valid_program(replacement)) {
        return Status::invalidProgram;
    }
    if (same_program(expected, replacement)) {
        return Status::invalidTransition;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (!record->mission.programBound || !same_program(record->mission.program, expected)) {
        status = Status::programMismatch;
    } else if (record->mission.faulted) {
        status = Status::invalidTransition;
    } else if (!record->mission.pendingIntents.empty()) {
        status = Status::intentMismatch;
    } else if (record->mission.inputSequence != record->mission.issuedInputSequence) {
        status = Status::inputSequenceMismatch;
    } else if (!publish(activity, *record)) {
        status = Status::revisionExhausted;
    } else {
        record->mission.program = replacement;
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Atomically issues the next durable per-binding Host mission-input sequence. */
bool issue_input_sequence(const SessionBinding& binding, std::uint64_t& output) noexcept {
    output = 0;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    bool issued = false;
    if (record != nullptr && activity.stateRevision < kMaximumRevision - 1) {
        const std::uint64_t highWater =
            (std::max)(record->mission.inputSequence, record->mission.issuedInputSequence);
        if (highWater != (std::numeric_limits<std::uint64_t>::max)()) {
            MissionState candidate{};
            if (copy_mission_state(record->mission, candidate)) {
                candidate.issuedInputSequence = highWater + 1;
                if (publish(activity, *record)) {
                    record->mission = std::move(candidate);
                    output = highWater + 1;
                    issued = true;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return issued;
}

/** Reads one exact binding's accepted-input cursor pair without mutating State. */
bool input_sequence_snapshot(const SessionBinding& binding,
                             InputSequenceSnapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const SessionRecord* const record = find_record(runtime::storage::g_state.activity, binding);
    const bool found = record != nullptr;
    if (found) {
        output.committed = record->mission.inputSequence;
        output.issued = record->mission.issuedInputSequence;
        output.faulted = record->mission.faulted;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Copies one exact mission record for a read-only view, changing nothing. */
bool state_snapshot(const SessionBinding& binding, Snapshot& output) noexcept {
    output = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const SessionRecord* const record = find_record(runtime::storage::g_state.activity, binding);
    const bool copied =
        record != nullptr
        && copy_snapshot(runtime::storage::g_state.activity, binding, *record, output);
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return copied;
}

/** Durably faults one exact binding even before a Mission program has attached. */
Status fault_input_feed(const SessionBinding& binding, Snapshot& output) noexcept {
    output = {};
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (!record->mission.faulted) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else if (!publish(activity, *record)) {
            status = Status::revisionExhausted;
        } else {
            candidate.faulted = true;
            record->mission = std::move(candidate);
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Retires every issued input of a binding no program is bound to. */
Status retire_unbound_inputs(const SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (record->mission.programBound) {
        status = Status::invalidTransition;
    } else if (record->mission.inputSequence != record->mission.issuedInputSequence) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else if (!publish(activity, *record)) {
            status = Status::revisionExhausted;
        } else {
            candidate.inputSequence = candidate.issuedInputSequence;
            record->mission = std::move(candidate);
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Commits one VM transaction against its exact prior mission revision. */
Status commit(const SessionBinding& binding,
              const ProgramKey& program,
              std::uint64_t expectedRevision,
              std::uint64_t expectedInputSequence,
              const CommitCandidate& transaction,
              Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    const bool revisionValid = transaction.nextRevision == expectedRevision
                               || (expectedRevision != (std::numeric_limits<std::uint64_t>::max)()
                                   && transaction.nextRevision == expectedRevision + 1);
    if (!revisionValid) {
        return Status::invalidTransition;
    }
    const bool inputSequenceValid =
        transaction.nextInputSequence == expectedInputSequence
        || (expectedInputSequence != (std::numeric_limits<std::uint64_t>::max)()
            && transaction.nextInputSequence == expectedInputSequence + 1
            && (expectedInputSequence != 0 || transaction.nextInputSequence == 1));
    if (!inputSequenceValid) {
        return Status::invalidTransition;
    }
    if (transaction.variables.size() > kVariableCapacity) {
        return Status::variableCapacity;
    }
    if (transaction.timers.size() > kTimerCapacity) {
        return Status::timerCapacity;
    }
    if (!valid_variables(transaction.variables)) {
        return Status::invalidVariable;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (!record->mission.programBound || !same_program(record->mission.program, program)) {
        status = Status::programMismatch;
    } else if (record->mission.revision != expectedRevision) {
        status = Status::revisionMismatch;
    } else if (record->mission.inputSequence != expectedInputSequence) {
        status = Status::inputSequenceMismatch;
    } else if (const Status transition = transition_status(record->mission, transaction);
               transition != Status::ready) {
        status = transition;
    } else {
        MissionState candidate{};
        status = merge_intents(
            record->mission, transaction.nextRevision, transaction.pendingIntents, candidate);
        if (status == Status::ready) {
            const bool missionChanged =
                record->mission.phase != transaction.phase
                || record->mission.started != transaction.started
                || record->mission.pendingIntents.size() != candidate.pendingIntents.size()
                || !same_variables(record->mission, transaction.variables)
                || !same_timers(record->mission, transaction.timers)
                || record->mission.nextTimerSequence != transaction.nextTimerSequence
                || record->mission.nextIntentKey != transaction.nextIntentKey;
            const bool revisionAdvanced = transaction.nextRevision != expectedRevision;
            if (missionChanged && !revisionAdvanced) {
                status = Status::invalidTransition;
            }
            const bool changed =
                status == Status::ready
                && (revisionAdvanced
                    || record->mission.inputSequence != transaction.nextInputSequence
                    || missionChanged);
            std::copy(transaction.variables.begin(),
                      transaction.variables.end(),
                      candidate.variables.begin());
            std::fill(candidate.variables.begin()
                          + static_cast<std::ptrdiff_t>(transaction.variables.size()),
                      candidate.variables.end(),
                      ScriptVariable{});
            std::copy(
                transaction.timers.begin(), transaction.timers.end(), candidate.timers.begin());
            std::fill(candidate.timers.begin()
                          + static_cast<std::ptrdiff_t>(transaction.timers.size()),
                      candidate.timers.end(),
                      MissionTimer{});
            candidate.revision = transaction.nextRevision;
            candidate.inputSequence = transaction.nextInputSequence;
            candidate.nextTimerSequence = transaction.nextTimerSequence;
            candidate.nextIntentKey = transaction.nextIntentKey;
            candidate.phase = transaction.phase;
            candidate.variableCount = transaction.variables.size();
            candidate.timerCount = transaction.timers.size();
            candidate.started = transaction.started;
            if (status == Status::ready && changed) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Marks an exact mission revision faulted without changing its phase. */
Status fault(const SessionBinding& binding,
             const ProgramKey& program,
             std::uint64_t expectedRevision,
             Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (!record->mission.programBound || !same_program(record->mission.program, program)) {
        status = Status::programMismatch;
    } else if (record->mission.revision != expectedRevision) {
        status = Status::revisionMismatch;
    } else if (!record->mission.faulted) {
        if (!publish(activity, *record)) {
            status = Status::revisionExhausted;
        } else {
            record->mission.faulted = true;
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** Clears one exact program fault when no durable output remains. */
Status recover(const SessionBinding& binding,
               const ProgramKey& program,
               std::uint64_t expectedRevision,
               Snapshot& output) noexcept {
    output = {};
    if (!valid_program(program)) {
        return Status::invalidProgram;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& activity = runtime::storage::g_state.activity;
    SessionRecord* const record = find_record(activity, binding);
    Status status = Status::ready;
    if (record == nullptr) {
        status = Status::invalidBinding;
    } else if (!record->mission.programBound || !same_program(record->mission.program, program)) {
        status = Status::programMismatch;
    } else if (record->mission.revision != expectedRevision) {
        status = Status::revisionMismatch;
    } else if (!record->mission.pendingIntents.empty()) {
        status = Status::intentMismatch;
    } else if (record->mission.faulted) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            candidate.faulted = false;
            candidate.issuedInputSequence = candidate.inputSequence;
            if (!publish(activity, *record)) {
                status = Status::revisionExhausted;
            } else {
                record->mission = std::move(candidate);
            }
        }
    }
    if (status == Status::ready && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/** @return Stable text for one mission-state result. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ready:
        return "ready";
    case Status::invalidBinding:
        return "invalid_binding";
    case Status::invalidProgram:
        return "invalid_program";
    case Status::programMismatch:
        return "program_mismatch";
    case Status::revisionMismatch:
        return "revision_mismatch";
    case Status::inputSequenceMismatch:
        return "input_sequence_mismatch";
    case Status::intentMismatch:
        return "intent_mismatch";
    case Status::outOfMemory:
        return "out_of_memory";
    case Status::intentSequenceExhausted:
        return "intent_sequence_exhausted";
    case Status::variableCapacity:
        return "variable_capacity";
    case Status::timerCapacity:
        return "timer_capacity";
    case Status::invalidVariable:
        return "invalid_variable";
    case Status::invalidTimer:
        return "invalid_timer";
    case Status::hostRevisionMismatch:
        return "host_revision_mismatch";
    case Status::revisionExhausted:
        return "revision_exhausted";
    case Status::invalidTransition:
        return "invalid_transition";
    }
    return "unknown";
}

} // namespace sunrise::state::activity::mission
