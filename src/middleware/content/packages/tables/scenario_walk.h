#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "scenario_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Which nested blob a read is for. Each slot needs its own storage to stay live. */
enum class ReadSlot : std::uint8_t {
    entry,
    registry,
    object,
    count,
};

/**
 * Reads one tag's bytes into the caller's storage for that slot.
 * Injecting the read keeps the walk provable without packages or block keys.
 */
using TagReader = bool (*)(void* context,
                           ReadSlot slot,
                           std::uint32_t tag,
                           std::span<const std::byte>& blob) noexcept;

/** One object placed in one slice-set state of one bubble. */
struct Placement {
    std::uint32_t bubbleHash{};
    std::uint32_t stateHash{};
    std::uint32_t entryTag{};
    std::uint32_t sliceSetIndex{};
    std::uint32_t registryTag{};
    std::uint32_t objectTag{};
    std::uint32_t objectKey{};
    std::uint64_t slotCount{};
    /**
     * The object's own bytes, valid only during the visitor call.
     * The next object read replaces them, so a consumer that needs the slots must read them here.
     */
    std::span<const std::byte> objectBytes{};
    std::uint64_t bubbleIndex{};
    std::uint64_t stateIndex{};
    std::size_t registryDescriptor{};
    std::uint64_t objectIndex{};
    bool stateEnabled{};
    bool bubblePublic{};
};

/** Totals from one scenario walk. */
struct WalkResult {
    std::uint64_t bubbles{};
    std::uint64_t states{};
    std::uint64_t entries{};
    std::uint64_t registries{};
    std::uint64_t placements{};
    /** States whose entry or registry did not read. The walk continues past them. */
    std::uint64_t unresolved{};
};

/** Visitor called once per placement. Returning false stops the walk and fails it. */
using PlacementVisitor = bool (*)(void* context, const Placement& placement) noexcept;

/**
 * Walks a scenario and emits one placement per object of every resolvable slice-set state.
 * A state whose entry or registry does not read is counted in `unresolved` and skipped.
 * @param blob Complete scenario bytes.
 * @param reader Tag reader supplying each nested blob.
 * @param visitor Placement consumer; returning false stops the walk and fails it.
 * @param output Cleared before the walk and filled as it runs.
 * @return True when every bubble, state and object read back and the visitor accepted them all.
 */
[[nodiscard]] bool walk_scenario(std::span<const std::byte> blob,
                                 TagReader reader,
                                 void* readerContext,
                                 PlacementVisitor visitor,
                                 void* visitorContext,
                                 WalkResult& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
