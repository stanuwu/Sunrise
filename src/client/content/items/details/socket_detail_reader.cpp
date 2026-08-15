#include "socket_detail_reader.h"

#include <cstddef>
#include <cstdint>

#include "layout.h"
#include "memory.h"
#include "relative.h"

namespace sunrise::client::content::items::details::socket_reader {
namespace {

namespace build_details = state::build_data::items::details;

/**
 * Finds and checks an ordinary-socket array header.
 * @param source Installed item-detail source.
 * @param block Loaded ordinary-socket block address.
 * @param field Receives the self-relative array field.
 * @param headerAddress Receives the array header address.
 * @param header Receives the whole fixed header prefix.
 * @return True when count, marker, repeated count, and element class match the ABI.
 */
[[nodiscard]] bool read_header(const investment::Source& source,
                               std::uintptr_t block,
                               layout::RelativeArray& field,
                               std::uintptr_t& headerAddress,
                               layout::ArrayHeader& header) noexcept {
    if (!memory::read_value(source, block, field)
        || field.count > static_cast<std::uint64_t>(build_details::kInitialPlugCapacity)) {
        return false;
    }

    std::uintptr_t relativeAddress = 0;
    std::uintptr_t markerAddress = 0;
    if (!memory::member_address(
            block, offsetof(layout::RelativeArray, headerRelative), relativeAddress)
        || !memory::add_signed_fits(relativeAddress, field.headerRelative, headerAddress)
        || !memory::add_signed_fits(headerAddress,
                                    -static_cast<std::int64_t>(layout::kArrayMarkerBackOffset),
                                    markerAddress)) {
        return false;
    }

    std::uint32_t marker = 0;
    return memory::read_value(source, markerAddress, marker) && marker == layout::kArrayHeaderMarker
           && memory::read_value(source, headerAddress, header)
           && header.repeatedCount == field.count
           && header.elementClass == layout::kOrdinarySocketElementClass;
}

} // namespace

/** Reads the optional ordinary-socket block and its initial plug indices. */
bool read_ordinary(const investment::Source& source,
                   std::uintptr_t definition,
                   std::size_t itemRowCount,
                   build_details::Definition& output) noexcept {
    bool present = false;
    std::uintptr_t block = 0;
    if (!relative::resolve_block(source,
                                 definition,
                                 offsetof(layout::ItemDefinition, ordinarySocketBlockRelative),
                                 present,
                                 block)) {
        return false;
    }
    if (!present) {
        output.ordinarySocketState = build_details::OrdinarySocketState::absent;
        output.ordinarySocketCount = 0;
        return true;
    }

    layout::RelativeArray field{};
    layout::ArrayHeader header{};
    std::uintptr_t headerAddress = 0;
    if (!read_header(source, block, field, headerAddress, header)) {
        return false;
    }

    std::uintptr_t entriesAddress = 0;
    if (!memory::member_address(headerAddress, sizeof(layout::ArrayHeader), entriesAddress)) {
        return false;
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(field.count); ++index) {
        std::uintptr_t entryOffset = 0;
        std::uintptr_t entryAddress = 0;
        layout::OrdinarySocketEntry entry{};
        if (!memory::multiply_fits(index, sizeof(layout::OrdinarySocketEntry), entryOffset)
            || !memory::add_fits(entriesAddress, entryOffset, entryAddress)
            || !memory::read_value(source, entryAddress, entry)
            || (entry.initialPlugIndex != build_details::kUnavailableItemIndex
                && static_cast<std::size_t>(entry.initialPlugIndex) >= itemRowCount)) {
            return false;
        }
        output.initialPlugIndices[index] = entry.initialPlugIndex;
    }
    output.ordinarySocketState = build_details::OrdinarySocketState::present;
    output.ordinarySocketCount = static_cast<std::uint8_t>(field.count);
    return true;
}

/** Reads the optional socket-entry-list row selection. */
bool read_list_index(const investment::Source& source,
                     std::uintptr_t definition,
                     std::size_t rowCount,
                     build_details::Definition& output) noexcept {
    bool present = false;
    std::uintptr_t block = 0;
    if (!relative::resolve_block(source,
                                 definition,
                                 offsetof(layout::ItemDefinition, socketListBlockRelative),
                                 present,
                                 block)) {
        return false;
    }
    if (!present) {
        output.socketEntryListIndex = build_details::kEmptySocketEntryListIndex;
        return true;
    }

    std::uint16_t index = 0;
    if (!memory::read_member(source, block, offsetof(layout::SocketListBlock, index), index)
        || static_cast<std::size_t>(index) >= rowCount) {
        return false;
    }
    output.socketEntryListIndex = index;
    return true;
}

} // namespace sunrise::client::content::items::details::socket_reader
