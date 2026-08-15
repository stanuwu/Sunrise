#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::hooks::bootflow::spawn {

/** Which gate condition refused, in the gate's own order. */
enum class Refusal : std::uint8_t {
    /** Nothing tested here refused, so the remaining dev-switch arm is what did. */
    devHold,
    /** The gate was not asked, or its predicates were not found. */
    unknown,
    /** G0: the caller passed the absent player datum. */
    datum,
    /** G1: no world is present. */
    world,
    /** G2: the current slice-set index is not addressable. */
    sliceSet,
    /** G3: the datum has no participation record at all. */
    participationMissing,
    /** G3: the record exists and its `+10` byte is clear. */
    participation,
    /** G4: the activity lifetime state is outside the passing set. */
    lifetime,
    /** G5: the world-controller handles are not ready. */
    worldControl,
    /** G6a: the team state or team block is absent. */
    teamMissing,
    /** G6b: the host published a nonzero team-state byte. */
    teamState,
    /** G6c: no team index is assigned. */
    teamIndex,
    /** G7: the host published the spawn-suppressed state. */
    suppressed,
    /** Nothing refused, so the gate allowed. */
    none,
};

/** Everything one probe pass read, so the log can name values, not just a result. */
struct Reading {
    Refusal refusal{Refusal::unknown};
    /** True when the datum has a participation record, which msg 5's type-13 body fills. */
    bool hasRecord{};
    /** Record `+8`, the step-36 task-9 latch. */
    std::uint8_t recordTask9{};
    /** Record `+10`, the byte G3 requires to be set. */
    std::uint8_t recordReady{};
    /** Activity lifetime state the gate switched on, or -1 when it was not read. */
    std::int32_t lifetimeState{-1};
    /** The team-state byte at `+1`, whose four low bits are the host's suppression reasons. */
    std::uint8_t teamStateBits{};
    /** The team index at identity `+9`; 0xFF means none was assigned. */
    std::uint8_t teamIndex{};
    /** True once the client reference manager holds a type-13 entry, which arms the team block. */
    bool hasType13{};
};

/**
 * Finds the gate's own predicates from the call operands inside it.
 * Every target is read out of the gate body, so this adds no signature of its own.
 * @param gate Base of the spawn gate, found by pattern.
 * @return True when every call site held the expected opcode and decoded.
 */
[[nodiscard]] bool resolve(const std::byte* gate) noexcept;

/** Clears the predicates it found. */
void forget() noexcept;

/**
 * Re-runs the gate's conditions in order and reports the first that refuses.
 * Calls only the gate's own read-only accessors, with its argument, on its thread and inside its
 * call, so it sees what the gate saw.
 * @param datum Player datum handle the gate was called with.
 * @return What the pass read; `unknown` when the predicates were not found.
 */
[[nodiscard]] Reading examine(std::int32_t datum) noexcept;

/**
 * Formats one reading as log fields.
 * @param reading Result of a probe pass.
 * @param output Caller-owned character storage.
 * @return Characters written, not counting the null.
 */
[[nodiscard]] std::size_t describe(const Reading& reading, std::span<char> output) noexcept;

} // namespace sunrise::client::hooks::bootflow::spawn
