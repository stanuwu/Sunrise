#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../unlocks/unlocks_expression.h"
#include "../items/details/definition.h"
#include "../items/item_catalog.h"

namespace sunrise::state::build_data::rewards {

/** Native references use all bits set for an absent row. */
inline constexpr std::uint16_t kAbsent = 0xFFFF;
/** The shipped build declares 979 pools. The domain leaves room above that. */
inline constexpr std::size_t kPoolCapacity = 4096;
/** Item rows are dense over the installed item-definition table. */
inline constexpr std::size_t kItemCapacity = items::kDefinitionCapacity;
/** The shipped build publishes 10,549 entries. The bank leaves room above that. */
inline constexpr std::size_t kEntryCapacity = 16384;
/** The shipped build publishes 14,185 expanded condition instructions, with room above that. */
inline constexpr std::size_t kInstructionCapacity = 16384;
/** The shipped build publishes 3,714 modifiers. The bank leaves room above that. */
inline constexpr std::size_t kModifierCapacity = 4096;
/** The shipped build publishes 1,812 socket overrides. The bank leaves room above that. */
inline constexpr std::size_t kSocketOverrideCapacity = 2048;
/** Reward overrides address the ordinary socket lanes an item's initial plugs fill. */
inline constexpr std::size_t kSocketsPerItem = items::details::kInitialPlugCapacity;
/** Category selections one wrapper declares. The most any shipped wrapper declares is four. */
inline constexpr std::size_t kSelectionCapacity = 4;
/** Grant rows one resolution may prepare: both change lists in full; perk rows use no list. */
inline constexpr std::size_t kGrantCapacity = 32;
/**
 * Nested pools, automatic wrappers and referenced conditions share one traversal depth. The
 * installed graph reaches 7.
 */
inline constexpr std::size_t kTraversalDepth = 32;

struct Range {
    std::uint32_t first{};
    std::uint32_t count{};
};

/** Reward and pass conditions are unlock expressions bound to their saved banks. */
using Instruction = unlocks::Instruction;

/** A fixed override names its plug directly rather than selecting from a set. */
inline constexpr std::uint32_t kFixedPlugSelection = 0xFFFFFFFFU;
/** Wrappers without this bit remain inventory items until explicitly opened. */
inline constexpr std::uint32_t kOpenOnAcquisition = 1;

struct SocketOverride {
    std::uint16_t socketType{kAbsent};
    std::uint16_t plugItem{kAbsent};
    std::uint16_t plugSet{kAbsent};
    std::uint16_t rollSet{kAbsent};
    std::uint32_t selection{kFixedPlugSelection};
};

struct Modifier {
    Range condition{};
    std::uint16_t valueIndex{kAbsent};
    float value{};
};

struct Entry {
    std::uint16_t itemIndex{kAbsent};
    std::uint16_t poolIndex{kAbsent};
    std::uint16_t supplementalIndex{kAbsent};
    /** True only when the source bank is explicitly absent from the investment root. */
    bool supplementalMissing{};
    std::uint32_t quantity{};
    std::uint32_t categoryHash{};
    float weight{};
    Range condition{};
    Range modifiers{};
    Range sockets{};
};

struct Pool {
    std::uint32_t definitionHash{};
    Range entries{};
};

struct Selection {
    std::uint32_t categoryHash{};
    std::uint32_t count{};
};

/** Dense item rows bind inventory definitions to their reward pools and acquisition flags. */
struct Item {
    std::uint32_t definitionHash{};
    std::uint16_t poolIndex{kAbsent};
    std::uint16_t acquiredFlag{kAbsent};
    std::array<Selection, kSelectionCapacity> selections{};
    std::uint8_t selectionCount{};
    std::uint32_t flags{};
};

struct View {
    std::span<const Pool> pools;
    std::span<const Entry> entries;
    std::span<const Item> items;
    std::span<const Instruction> instructions;
    std::span<const Modifier> modifiers;
    std::span<const SocketOverride> sockets;
};

template <typename T> [[nodiscard]] constexpr bool fits(Range range, std::span<T> bank) noexcept {
    return range.first <= bank.size() && range.count <= bank.size() - range.first;
}

/** An override names a socket type and, when it fixes a plug, an item that was read. */
[[nodiscard]] constexpr bool valid_socket(const SocketOverride& socket,
                                          std::size_t itemCount) noexcept {
    return socket.socketType != kAbsent
           && (socket.plugItem == kAbsent || socket.plugItem < itemCount);
}

} // namespace sunrise::state::build_data::rewards
