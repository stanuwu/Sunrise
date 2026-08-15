#pragma once

#include <cstddef>
#include <cstdint>

#include "memory.h"

namespace sunrise::client::content::items::details::relative {

/**
 * Finds one optional self-relative definition member.
 * @param source Installed item-detail source.
 * @param definition Loaded item definition address.
 * @param memberOffset Byte offset of the signed relative member.
 * @param present Receives whether the relative member is nonzero.
 * @param block Receives the block address when present.
 * @return True when the member reads and any nonzero offset is safe.
 */
[[nodiscard]] inline bool resolve_block(const investment::Source& source,
                                        std::uintptr_t definition,
                                        std::size_t memberOffset,
                                        bool& present,
                                        std::uintptr_t& block) noexcept {
    present = false;
    block = 0;
    std::int64_t displacement = 0;
    std::uintptr_t memberAddress = 0;
    if (!memory::member_address(definition, memberOffset, memberAddress)
        || !memory::read_value(source, memberAddress, displacement)) {
        return false;
    }
    if (displacement == 0) {
        return true;
    }
    present = true;
    return memory::add_signed_fits(memberAddress, displacement, block);
}

} // namespace sunrise::client::content::items::details::relative
