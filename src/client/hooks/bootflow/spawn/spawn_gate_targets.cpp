#include <cstddef>

#include "../../../patterns/image_scan.h"
#include "probe.h"
#include "spawn_gate_record_dump.h"

namespace sunrise::client::hooks::bootflow::spawn {
namespace {

/** Opcode of the near call every site must hold. It proves the offset is still right. */
constexpr std::byte kCallOpcode{0xE8};
/** Length of a near call, and the offset of its displacement. */
constexpr std::size_t kCallLength = 5;
constexpr std::size_t kCallOperand = 1;

/**
 * Byte offsets of each predicate's call, from the gate's own base, in gate order.
 * Each site is checked for the call opcode before its operand is decoded, so a moved body
 * refuses the probe instead of calling a wrong address.
 */
struct Site {
    /** The slice-set manager accessor both world conditions start from. */
    static constexpr std::size_t sliceSetManager = 0x23;
    /** G1: a world is present. */
    static constexpr std::size_t worldPresent = 0x2B;
    /** G2's input: the current slice-set index. */
    static constexpr std::size_t currentSliceSet = 0x49;
    /** G2: that index is addressable. */
    static constexpr std::size_t sliceSetAddressable = 0x51;
    /** G3: the participation record message 5's type-13 body binds. */
    static constexpr std::size_t participationRecord = 0x83;
    /** G4: the activity lifetime state the jump table switches on. */
    static constexpr std::size_t lifetimeState = 0xC5;
    /** G5: the world-controller handles are ready. */
    static constexpr std::size_t worldControl = 0xF0;
    /** G6: a type-13 client reference exists, which arms the whole team block. */
    static constexpr std::size_t hasAnyType13 = 0x100;
    /** G6c's input: the identity block carrying the team index. */
    static constexpr std::size_t identity = 0x10F;
    /** G6a and G6b's input: the team state whose byte 1 holds the suppression bits. */
    static constexpr std::size_t teamState = 0x119;
    /** G6a's second input: the team block. */
    static constexpr std::size_t teamBlock = 0x123;
    /** G7: the host published the spawn-suppressed state. */
    static constexpr std::size_t spawnSuppressed = 0x197;
};

/**
 * Decodes one call target, refusing unless the site really holds a near call.
 * @param offset Byte offset of the call from the gate base.
 * @return The called address, or null when the site is not a near call.
 */
[[nodiscard]] void* call_target(const std::byte* gate, std::size_t offset) noexcept {
    const std::byte* const site = gate + offset;
    if (*site != kCallOpcode) {
        return nullptr;
    }
    return patterns::resolve_relative(site + kCallOperand, site + kCallLength);
}

/** @return True when every predicate decoded. */
[[nodiscard]] bool complete(const Predicates& calls) noexcept {
    return calls.sliceSetManager != nullptr && calls.worldPresent != nullptr
           && calls.currentSliceSet != nullptr && calls.sliceSetAddressable != nullptr
           && calls.participationRecord != nullptr && calls.lifetimeState != nullptr
           && calls.worldControl != nullptr && calls.hasAnyType13 != nullptr
           && calls.identity != nullptr && calls.teamState != nullptr && calls.teamBlock != nullptr
           && calls.spawnSuppressed != nullptr;
}

} // namespace

/** Finds the gate's own predicates from the call operands inside it. */
bool resolve(const std::byte* gate) noexcept {
    g_ready.store(false, std::memory_order_release);
    if (gate == nullptr) {
        return false;
    }
    Predicates calls{};
    calls.sliceSetManager =
        reinterpret_cast<NoArgPointer>(call_target(gate, Site::sliceSetManager));
    calls.worldPresent = reinterpret_cast<PointerPredicate>(call_target(gate, Site::worldPresent));
    calls.currentSliceSet =
        reinterpret_cast<SliceSetIndexGetter>(call_target(gate, Site::currentSliceSet));
    calls.sliceSetAddressable =
        reinterpret_cast<PointerPredicate>(call_target(gate, Site::sliceSetAddressable));
    calls.participationRecord =
        reinterpret_cast<DatumPointer>(call_target(gate, Site::participationRecord));
    calls.lifetimeState = reinterpret_cast<OutPredicate>(call_target(gate, Site::lifetimeState));
    calls.worldControl = reinterpret_cast<NoArgPredicate>(call_target(gate, Site::worldControl));
    calls.hasAnyType13 = reinterpret_cast<NoArgPredicate>(call_target(gate, Site::hasAnyType13));
    calls.identity = reinterpret_cast<DatumPointer>(call_target(gate, Site::identity));
    calls.teamState = reinterpret_cast<DatumPointer>(call_target(gate, Site::teamState));
    calls.teamBlock = reinterpret_cast<DatumPointer>(call_target(gate, Site::teamBlock));
    calls.spawnSuppressed =
        reinterpret_cast<NoArgPredicate>(call_target(gate, Site::spawnSuppressed));
    if (!complete(calls)) {
        return false;
    }
    g_calls = calls;
    g_ready.store(true, std::memory_order_release);
    return true;
}

/** Clears the predicates it found. */
void forget() noexcept {
    g_ready.store(false, std::memory_order_release);
    g_calls = {};
}

} // namespace sunrise::client::hooks::bootflow::spawn
