#include "instance_encoder.h"

#include <algorithm>
#include <cstring>

#include "abi.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::instance {
namespace {

/**
 * Checks the installed table bounds for one item or plug definition index.
 * @param index Candidate native item-definition index.
 * @param count Installed item-definition table row count.
 * @return True when the index names a row in the installed table.
 */
[[nodiscard]] bool valid_item_definition(std::uint16_t index, std::uint32_t count) noexcept {
    return count != 0 && count <= layout::kDefinitionIndexCapacity
           && index != layout::kEmptyDefinitionIndex && index < count;
}

/**
 * Validates optional ordinary plug indices against the installed item table.
 * @param sockets Optional ordinary socket block from resolved State.
 * @param itemDefinitionCount Installed item-definition table row count.
 * @return True when every present plug names an installed item definition.
 */
[[nodiscard]] bool valid_ordinary_sockets(const OrdinarySockets& sockets,
                                          std::uint32_t itemDefinitionCount) noexcept {
    if (sockets.state == OrdinarySocketBlockState::unresolved) {
        return false;
    }
    if (sockets.state == OrdinarySocketBlockState::absent) {
        return std::none_of(
            sockets.plugs.begin(),
            sockets.plugs.end(),
            [](const std::optional<std::uint16_t>& plug) { return plug.has_value(); });
    }
    if (sockets.state != OrdinarySocketBlockState::present) {
        return false;
    }
    return std::all_of(sockets.plugs.begin(),
                       sockets.plugs.end(),
                       [itemDefinitionCount](const std::optional<std::uint16_t>& plug) {
                           return !plug.has_value()
                                  || valid_item_definition(*plug, itemDefinitionCount);
                       });
}

/**
 * Validates the socket-entry-list row and its initial state lanes.
 * @param input Fully resolved candidate item instance.
 * @return True when the row is installed and every lane uses a safe initial state.
 */
[[nodiscard]] bool valid_socket_entry_mapping(const ResolvedInstance& input) noexcept {
    if (!input.socketEntryContentsResolved
        || input.socketEntryListIndex == layout::kEmptyDefinitionIndex
        || input.bounds.socketEntryListCount == 0
        || input.bounds.socketEntryListCount > layout::kDefinitionIndexCapacity
        || input.socketEntryListIndex >= input.bounds.socketEntryListCount
        || input.socketEntryCount > layout::kSocketEntryStateCapacity) {
        return false;
    }

    for (std::size_t index = 0; index < input.socketEntryStates.size(); ++index) {
        const SocketEntryState state = input.socketEntryStates[index];
        if (state != SocketEntryState::absent && state != SocketEntryState::ready
            && state != SocketEntryState::active) {
            return false;
        }
        if (index >= input.socketEntryCount && state != SocketEntryState::absent) {
            return false;
        }
    }
    // A selector may only name an entry the list actually has.
    for (const SocketSelector& selector : input.socketSelectors) {
        if (selector.entry != layout::kEmptySelectorByte
            && selector.entry >= input.socketEntryCount) {
            return false;
        }
    }

    return true;
}

/**
 * Validates every resolved mapping before any caller-owned output is changed.
 * @param input Candidate resolved item instance.
 * @return True when the record can be encoded without an unresolved native lookup.
 */
[[nodiscard]] bool valid(const ResolvedInstance& input) noexcept {
    return input.instanceSoid != 0 && input.level >= layout::kMinimumItemLevel
           && input.curveSelector == layout::kInitialLevelCurveX
           && input.capSelector == layout::kInitialLevelCapRow
           && valid_item_definition(input.baseDefinitionIndex, input.bounds.itemDefinitionCount)
           && valid_ordinary_sockets(input.ordinarySockets, input.bounds.itemDefinitionCount)
           && valid_socket_entry_mapping(input);
}

/**
 * Applies every biased-index and definition-hash sentinel required by an existing item.
 * @param object Zero-initialized native item-instance object.
 */
void initialize_empty_fields(layout::Object& object) noexcept {
    for (layout::OrdinarySocket& socket : object.ordinarySockets.sockets) {
        socket.plugDefinitionIndex = layout::kEmptyDefinitionIndex;
        socket.auxiliaryHashes.fill(layout::kSafeSocketAuxiliaryHash);
    }
    object.roll.progressionDefinitionIndex = layout::kEmptyDefinitionIndex;
    for (layout::SocketSelector& selector : object.roll.selectors) {
        selector.entry = layout::kEmptySelectorByte;
        selector.group = layout::kEmptySelectorByte;
        selector.element = layout::kEmptySelectorByte;
    }
    object.creation.primaryDefinitionIndex = layout::kEmptyDefinitionIndex;
    object.creation.definitionHashes.fill(layout::kNoDefinitionHash);
    object.creation.secondaryDefinitionIndex = layout::kEmptyDefinitionIndex;
    for (layout::CreationEntry& entry : object.creation.entries) {
        entry.definitionIndices.fill(layout::kEmptyDefinitionIndex);
    }
    object.tailDefinitionIndex = layout::kEmptyDefinitionIndex;
}

} // namespace

/** Encodes one item-instance object after validating all runtime-derived indices. */
bool encode(const ResolvedInstance& input, std::span<std::byte> output) noexcept {
    if (!valid(input) || output.size() < layout::kObjectSize) {
        return false;
    }

    layout::Object object{};
    initialize_empty_fields(object);
    object.instanceSoid = input.instanceSoid;
    object.level.level = input.level;
    object.level.curveX = input.curveSelector;
    object.level.capRow = input.capSelector;
    object.baseDefinitionIndex = input.baseDefinitionIndex;
    object.ordinarySockets.gateMask = layout::kAllSocketBits;
    object.roll.progress = layout::kInitialInstanceProgress;
    object.roll.socketEntryListIndex = input.socketEntryListIndex;

    if (input.ordinarySockets.state == OrdinarySocketBlockState::present) {
        // Both permit masks are filled. The plug walk reads the definition's declared plugs as
        // well as this instance's lanes, and skips whichever source its mask leaves unset.
        object.ordinarySockets.activeMask = layout::kAllSocketBits;
        object.ordinarySockets.definitionUnlockMask = layout::kAllSocketBits;
        for (std::size_t index = 0; index < input.ordinarySockets.plugs.size(); ++index) {
            const std::optional<std::uint16_t>& plug = input.ordinarySockets.plugs[index];
            if (plug.has_value()) {
                object.ordinarySockets.sockets[index].plugDefinitionIndex = *plug;
            }
        }
    }
    std::transform(input.socketEntryStates.begin(),
                   input.socketEntryStates.end(),
                   object.roll.socketEntryStates.begin(),
                   [](SocketEntryState state) { return static_cast<std::uint8_t>(state); });
    // A selection overwrites only the buckets it publishes; the rest keep the empty sentinel.
    for (std::size_t bucket = 0; bucket < object.roll.selectors.size(); ++bucket) {
        const SocketSelector& source = input.socketSelectors[bucket];
        if (source.entry == layout::kEmptySelectorByte) {
            continue;
        }
        object.roll.selectors[bucket] =
            layout::SocketSelector{source.entry, source.group, source.element};
    }

    // Commit only after validation so a rejected mapping leaves caller-owned storage unchanged.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::instance
