#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "layout.h"

namespace sunrise::middleware::datagen::family4::instance {

/** Native socket-entry states safe for an initial replicated item record. */
enum class SocketEntryState : std::uint8_t {
    /** No plug source exists for this socket-entry-list entry. */
    absent = 0,
    /** The entry exists but has not received a runtime selection. */
    ready = 16,
    /** The character has selected this entry, or it is the super lane. */
    active = 18,
};

/** One selector lane naming the socket entry a semantic ability bucket publishes. */
struct SocketSelector {
    std::uint8_t entry{0xFF};
    std::uint8_t group{0xFF};
    std::uint8_t element{0xFF};
};

/** Resolution state for the base definition's optional ordinary socket block. */
enum class OrdinarySocketBlockState : std::uint8_t {
    /** No runtime mapping has established whether the block exists. */
    unresolved = 0,
    /** The base definition has no ordinary socket block. */
    absent = 1,
    /** The base definition has a resolved ordinary socket block. */
    present = 2,
};

/** Explicitly resolved ordinary socket-block presence and optional plug indices. */
struct OrdinarySockets {
    OrdinarySocketBlockState state{OrdinarySocketBlockState::unresolved};
    std::array<std::optional<std::uint16_t>, layout::kOrdinarySocketCapacity> plugs{};
};

/** Runtime table bounds required to prove every supplied native index is valid. */
struct DefinitionBounds {
    std::uint32_t itemDefinitionCount{};
    std::uint32_t socketEntryListCount{};
};

/** Fully resolved semantic and native data required by one item-instance record. */
struct ResolvedInstance {
    std::uint64_t instanceSoid{};
    DefinitionBounds bounds{};
    std::uint16_t baseDefinitionIndex{layout::kEmptyDefinitionIndex};
    std::int32_t level{};
    /** Runtime-scored level curve written into the same native instance prefix. */
    std::uint16_t curveSelector{layout::kInitialLevelCurveX};
    /** Runtime-scored level cap written into the same native instance prefix. */
    std::uint16_t capSelector{layout::kInitialLevelCapRow};
    std::uint16_t socketEntryListIndex{layout::kEmptyDefinitionIndex};
    std::uint8_t socketEntryCount{};
    bool socketEntryContentsResolved{};
    OrdinarySockets ordinarySockets{};
    std::array<SocketEntryState, layout::kSocketEntryStateCapacity> socketEntryStates{};
    /** Selector lane per semantic ability bucket, empty for an item with no selection. */
    std::array<SocketSelector, layout::kSelectorCapacity> socketSelectors{};
};

/**
 * Encodes one item-instance object after validating all runtime-derived indices.
 * @param input Fully resolved item identity, sockets, initial states, and native table bounds.
 * @param output Exact runtime-mapped item-instance object storage.
 * @return True when every mapping is complete and the output span fits the native layout.
 */
[[nodiscard]] bool encode(const ResolvedInstance& input, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::datagen::family4::instance
