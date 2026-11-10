#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <span>

#include "../../runtime/storage/internal.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::state::activity::mission {
namespace {

/** @return True when the report names the actor of the step receipt. */
[[nodiscard]] bool reports_receipt(const CarrierOperation& carrier,
                                   const CarrierActorReport& report) noexcept {
    const CarrierReceipt& receipt = carrier.receipt;
    return receipt.requestKey == carrier.stepRequestKey && receipt.objectTag == report.objectTag
           && receipt.registryKey == report.registryKey && receipt.slotIndex == report.slotIndex
           && receipt.actorGeneration == report.actorGeneration;
}

/**
 * Applies one report to the step the carrier waits on.
 * @return True when the carrier changed.
 */
[[nodiscard]] bool observe_step(CarrierOperation& carrier,
                                CarrierWait wait,
                                const CarrierActorReport& report) noexcept {
    const CarrierReceipt& receipt = carrier.receipt;
    const bool echoed = wait == CarrierWait::delivery
                            ? report.deliveryKnown && report.deliveryRevision == receipt.revision
                            : report.programRevision == receipt.revision;
    if (!echoed) {
        return false;
    }
    // Only a step that retires the actor can finish on a suppressed one; any other wait would
    // never end, so the flight does.
    if (wait != CarrierWait::programRetires && carrier.observed && report.suppressed) {
        carrier.status = CarrierStatus::cancelled;
        return true;
    }
    const bool ran = !report.suppressed && report.programProgress >= receipt.laneCount;
    bool finished = false;
    switch (wait) {
    case CarrierWait::program:
        finished = ran;
        break;
    case CarrierWait::programRetires:
        // An ability that retires the actor suppresses it instead of finishing its lane.
        finished = ran || (carrier.observed && report.suppressed);
        break;
    case CarrierWait::delivery:
        finished = report.deliveryState == kCarrierDeliveryIdle;
        break;
    case CarrierWait::transport:
        break;
    }
    const bool changed = !carrier.observed || finished != carrier.stepFinished;
    carrier.observed = true;
    carrier.stepFinished = finished;
    return changed;
}

/** Ends a flight that can no longer run; @return True when it was running. */
[[nodiscard]] bool end_flight(CarrierOperation& carrier, CarrierStatus status) noexcept {
    if (carrier.status != CarrierStatus::running) {
        return false;
    }
    carrier.status = status;
    return true;
}

/**
 * Moves one running carrier past a finished step and queues its next one.
 * @return intentSequenceExhausted when the queue has no key or sequence left.
 */
[[nodiscard]] Status advance_flight(MissionState& candidate,
                                    CarrierOperation& carrier,
                                    bool& changed) noexcept {
    const auto& steps = carrier.plan->steps;
    if (carrier.stepRequestKey != 0) {
        const CarrierWait wait = steps[carrier.step].wait;
        if (!carrier.transported || (wait != CarrierWait::transport && !carrier.stepFinished)) {
            return Status::ready;
        }
        ++carrier.step;
        carrier.stepRequestKey = 0;
        carrier.receipt = {};
        carrier.transported = carrier.observed = carrier.stepFinished = false;
        changed = true;
    }
    if (carrier.step == steps.size()) {
        carrier.status = CarrierStatus::complete;
        changed = true;
        return Status::ready;
    }
    const CarrierStep& step = steps[carrier.step];
    carrier.delivered =
        step.stage == CarrierStage::departure || step.stage == CarrierStage::retirement;
    if (candidate.nextIntentKey == kAbsentIntentKey
        || candidate.nextIntentKey == (std::numeric_limits<std::uint64_t>::max)()) {
        return Status::intentSequenceExhausted;
    }
    TypedIntent intent{};
    try {
        intent = step.intent;
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    intent.requestKey = candidate.nextIntentKey++;
    intent.attemptGeneration = candidate.attempt.generation;
    carrier.stepRequestKey = intent.requestKey;
    changed = true;
    return append_intent(candidate, intent, candidate.revision);
}

/**
 * Drops the queued steps of flights that ended, unless the Host already holds the step's output,
 * and forgets each ended flight's step once it left the queue.
 * @return True when the queue or a flight changed.
 */
[[nodiscard]] bool retire_ended_steps(MissionState& state) {
    const auto ended = [&state](std::uint64_t requestKey) noexcept {
        return std::any_of(state.carriers.begin(),
                           state.carriers.end(),
                           [requestKey](const CarrierOperation& carrier) {
                               return carrier.status != CarrierStatus::running
                                      && carrier.stepRequestKey == requestKey;
                           });
    };
    bool changed = std::erase_if(state.pendingIntents, [&ended](const PendingIntent& pending) {
                       return pending.hostOutputRevision == kAbsentHostOutputRevision
                              && ended(pending.value.requestKey);
                   }) != 0;
    for (CarrierOperation& carrier : state.carriers) {
        const bool queued =
            std::any_of(state.pendingIntents.begin(),
                        state.pendingIntents.end(),
                        [&carrier](const PendingIntent& pending) {
                            return pending.value.requestKey == carrier.stepRequestKey;
                        });
        if (carrier.status != CarrierStatus::running && carrier.stepRequestKey != 0 && !queued) {
            carrier.stepRequestKey = 0;
            changed = true;
        }
    }
    return changed;
}

} // namespace

/**
 * A step leaving the queue: transport admits it, refusal refuses its flight. An accepted start
 * opens a flight; an accepted cancel ends the actor's flight.
 */
Status settle_carrier_intent(MissionState& candidate,
                             const TypedIntent& intent,
                             bool accepted) noexcept {
    for (CarrierOperation& carrier : candidate.carriers) {
        if (carrier.status != CarrierStatus::running) {
            continue;
        }
        if (carrier.stepRequestKey == intent.requestKey) {
            carrier.transported = accepted;
            if (!accepted) {
                carrier.status = CarrierStatus::refused;
            }
        } else if (accepted && intent.kind == IntentKind::cancelCarrier
                   && carrier.actorRow == intent.firstRow) {
            carrier.status = CarrierStatus::cancelled;
        }
    }
    if (!accepted || intent.kind != IntentKind::startCarrier) {
        return Status::ready;
    }
    CarrierOperation carrier{};
    carrier.plan = intent.carrierPlan;
    carrier.requestKey = intent.requestKey;
    carrier.attemptGeneration = intent.attemptGeneration;
    carrier.actorRow = intent.firstRow;
    try {
        candidate.carriers.push_back(std::move(carrier));
    } catch (const std::bad_alloc&) {
        return Status::outOfMemory;
    }
    return Status::ready;
}

/**
 * Joins only fresh accepted actor levels to the step each running carrier waits on. The link that
 * simulates the actor reports for it, so an admitted report from another link takes the flight
 * over: that link's client now flies the actor. Its own reports start the step's join again.
 */
Status observe_carrier_actor(const SessionBinding& binding,
                             const ProgramKey& program,
                             const CarrierActorReport& report,
                             Snapshot& output,
                             bool& changed) noexcept {
    output = {};
    changed = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto* record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    if (status == Status::ready
        && (!record->mission.programBound || !same_program(record->mission.program, program))) {
        status = Status::programMismatch;
    }
    if (status == Status::ready
        && (record->mission.faulted || report.inputSequence == 0
            || report.inputSequence > record->mission.issuedInputSequence)) {
        status = Status::invalidTransition;
    }
    if (status == Status::ready && report.attemptGeneration == record->mission.attempt.generation) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        } else {
            for (CarrierOperation& carrier : candidate.carriers) {
                if (carrier.status != CarrierStatus::running || !carrier.transported
                    || carrier.stepRequestKey == 0 || !reports_receipt(carrier, report)) {
                    continue;
                }
                if (carrier.sourceGeneration != report.sourceGeneration) {
                    if (!report.admitted || report.sourceGeneration == 0) {
                        continue;
                    }
                    carrier.sourceGeneration = report.sourceGeneration;
                    carrier.observed = carrier.stepFinished = false;
                    changed = true;
                }
                if (report.inputSequence <= carrier.receipt.inputSequenceAtStage
                    || report.clientMessageSequence
                           <= carrier.receipt.clientMessageSequenceAtStage) {
                    continue;
                }
                const CarrierWait wait = carrier.plan->steps[carrier.step].wait;
                if (wait != CarrierWait::transport && observe_step(carrier, wait, report)) {
                    changed = true;
                }
            }
            if (changed) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        }
    }
    if (status == Status::ready && changed && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

/**
 * Ends flights that lost their attempt or their link, moves each running flight past its finished
 * step, and queues its next step behind the program's own intents.
 */
Status advance_carriers(const SessionBinding& binding,
                        const ProgramKey& program,
                        std::span<const std::uint64_t> liveGenerations,
                        Snapshot& output,
                        bool& changed) noexcept {
    output = {};
    changed = false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& activity = runtime::storage::g_state.activity;
    auto* record = find_record(activity, binding);
    Status status = record == nullptr ? Status::invalidBinding : Status::ready;
    if (status == Status::ready
        && (!record->mission.programBound || record->mission.faulted
            || !same_program(record->mission.program, program))) {
        status = Status::programMismatch;
    }
    if (status == Status::ready) {
        MissionState candidate{};
        if (!copy_mission_state(record->mission, candidate)) {
            status = Status::outOfMemory;
        }
        try {
            for (CarrierOperation& carrier : candidate.carriers) {
                if (status != Status::ready || carrier.status != CarrierStatus::running) {
                    continue;
                }
                // A flight never adopts another attempt's receipts, and its actor leaves with the
                // link whose client flew it.
                if (carrier.attemptGeneration != candidate.attempt.generation
                    || candidate.attempt.complete
                    || (carrier.sourceGeneration != 0
                        && std::find(liveGenerations.begin(),
                                     liveGenerations.end(),
                                     carrier.sourceGeneration)
                               == liveGenerations.end())) {
                    changed = end_flight(carrier, CarrierStatus::cancelled) || changed;
                    continue;
                }
                status = advance_flight(candidate, carrier, changed);
            }
            // A cancel or refusal ends a flight outside this pass; its queued step goes here.
            if (status == Status::ready && retire_ended_steps(candidate)) {
                changed = true;
            }
            // state:operation reads only the current attempt's flights.
            if (status == Status::ready
                && std::erase_if(candidate.carriers, [&candidate](const CarrierOperation& ended) {
                       return ended.status != CarrierStatus::running && ended.stepRequestKey == 0
                              && ended.attemptGeneration != candidate.attempt.generation;
                   }) != 0) {
                changed = true;
            }
            if (status == Status::ready && changed) {
                if (!publish(activity, *record)) {
                    status = Status::revisionExhausted;
                } else {
                    record->mission = std::move(candidate);
                }
            }
        } catch (const std::bad_alloc&) {
            status = Status::outOfMemory;
        }
    }
    if (status == Status::ready && changed && !copy_snapshot(activity, binding, *record, output)) {
        status = Status::outOfMemory;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return status;
}

} // namespace sunrise::state::activity::mission
