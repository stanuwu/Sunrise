#include <array>
#include <cstdio>

#include "probe.h"
#include "spawn_gate_record_dump.h"

namespace sunrise::client::hooks::bootflow::spawn {
namespace {

/** The absent player datum the gate rejects outright. */
constexpr std::int32_t kAbsentDatum = -1;
/** Participation record byte the gate needs set. */
constexpr std::size_t kRecordReadyOffset = 10;
/** Participation record byte step 36's task 9 reads, carried on the same body. */
constexpr std::size_t kRecordTask9Offset = 8;
/** Identity byte holding the team index, and the value meaning none was assigned. */
constexpr std::size_t kIdentityTeamOffset = 9;
constexpr std::uint8_t kNoTeamIndex = 0xFF;
/** Team-state byte offset; every set bit is a host-published suppression reason. */
constexpr std::size_t kTeamStateOffset = 1;
/** Lifetime states that reach the passing arm of the gate's jump table. */
constexpr std::array<std::int32_t, 3> kPassingLifetimeStates{3, 6, 10};

/** Log names for each refusal, in the enum's own order. */
constexpr std::array<const char*, 14> kRefusalNames{"dev_hold",
                                                    "unknown",
                                                    "datum",
                                                    "world",
                                                    "slice_set",
                                                    "no_record",
                                                    "participation",
                                                    "lifetime",
                                                    "world_control",
                                                    "team_missing",
                                                    "team_state",
                                                    "team_index",
                                                    "suppressed",
                                                    "none"};

/** @return True when the lifetime state reaches the passing arm. */
[[nodiscard]] bool passing_lifetime(std::int32_t state) noexcept {
    for (const std::int32_t passing : kPassingLifetimeStates) {
        if (state == passing) {
            return true;
        }
    }
    return false;
}

/**
 * Runs the team block, which the gate reaches only once a type-13 entry exists.
 * @param reading Receives the team bytes read.
 * @return The refusal, or none when every team condition passes.
 */
[[nodiscard]] Refusal examine_team(std::int32_t datum, Reading& reading) noexcept {
    const std::uint8_t* const identity = g_calls.identity(datum);
    const std::uint8_t* const teamState = g_calls.teamState(datum);
    const std::uint8_t* const teamBlock = g_calls.teamBlock(datum);
    if (teamState == nullptr || teamBlock == nullptr) {
        return Refusal::teamMissing;
    }
    reading.teamStateBits = teamState[kTeamStateOffset];
    if (reading.teamStateBits != 0) {
        return Refusal::teamState;
    }
    reading.teamIndex = identity != nullptr ? identity[kIdentityTeamOffset] : kNoTeamIndex;
    return reading.teamIndex == kNoTeamIndex ? Refusal::teamIndex : Refusal::none;
}

} // namespace

/** Re-runs the gate's conditions in order and reports the first that refuses. */
Reading examine(std::int32_t datum) noexcept {
    Reading reading{};
    if (!g_ready.load(std::memory_order_acquire)) {
        return reading;
    }
    // Read the type-13 and type-17 witnesses before the ordered walk, because the walk returns at
    // the first refusal and these say which auth body applied. The lifetime state is the type-17
    // body's own value; the reference is what the type-13 roster slot registers.
    std::int32_t witness = 0;
    reading.lifetimeState = g_calls.lifetimeState(&witness) ? witness : -1;
    reading.hasType13 = g_calls.hasAnyType13();
    if (datum == kAbsentDatum) {
        reading.refusal = Refusal::datum;
        return reading;
    }
    if (!g_calls.worldPresent(g_calls.sliceSetManager())) {
        reading.refusal = Refusal::world;
        return reading;
    }
    std::int32_t sliceSetIndex = 0;
    void* const current = g_calls.currentSliceSet(g_calls.sliceSetManager(), &sliceSetIndex);
    if (!g_calls.sliceSetAddressable(current)) {
        reading.refusal = Refusal::sliceSet;
        return reading;
    }
    const std::uint8_t* const record = g_calls.participationRecord(datum);
    reading.hasRecord = record != nullptr;
    if (record == nullptr) {
        reading.refusal = Refusal::participationMissing;
        return reading;
    }
    dump_record(record);
    reading.recordTask9 = record[kRecordTask9Offset];
    reading.recordReady = record[kRecordReadyOffset];
    if (reading.recordReady == 0) {
        reading.refusal = Refusal::participation;
        return reading;
    }
    if (reading.lifetimeState < 0 || !passing_lifetime(reading.lifetimeState)) {
        reading.refusal = Refusal::lifetime;
        return reading;
    }
    if (!g_calls.worldControl()) {
        reading.refusal = Refusal::worldControl;
        return reading;
    }
    if (reading.hasType13) {
        const Refusal team = examine_team(datum, reading);
        if (team != Refusal::none) {
            reading.refusal = team;
            return reading;
        }
    }
    if (g_calls.spawnSuppressed()) {
        reading.refusal = Refusal::suppressed;
        return reading;
    }
    // Everything this probe can test passed, so the remaining dev-switch arm is what refused.
    reading.refusal = Refusal::devHold;
    return reading;
}

/** Formats one reading as log fields. */
std::size_t describe(const Reading& reading, std::span<char> output) noexcept {
    if (output.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(reading.refusal);
    const char* const name = index < kRefusalNames.size() ? kRefusalNames[index] : "unknown";
    const int written = std::snprintf(output.data(),
                                      output.size(),
                                      "reason=%s rec=%u rec8=%u rec10=%u lifetime=%d type13=%u "
                                      "team_bits=0x%02X "
                                      "faction=%c sync=%c picker=%c side=%c team_idx=0x%02X",
                                      name,
                                      reading.hasRecord ? 1U : 0U,
                                      reading.recordTask9,
                                      reading.recordReady,
                                      reading.lifetimeState,
                                      reading.hasType13 ? 1U : 0U,
                                      reading.teamStateBits,
                                      (reading.teamStateBits & 1U) != 0 ? 'Y' : 'N',
                                      (reading.teamStateBits & 2U) != 0 ? 'Y' : 'N',
                                      (reading.teamStateBits & 4U) != 0 ? 'Y' : 'N',
                                      (reading.teamStateBits & 8U) != 0 ? 'Y' : 'N',
                                      reading.teamIndex);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

} // namespace sunrise::client::hooks::bootflow::spawn
