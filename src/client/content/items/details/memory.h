#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../investment/source.h"
#include "../layout.h"

namespace sunrise::client::content::items::details::memory {

[[nodiscard]] inline bool
add_fits(std::uintptr_t left, std::uintptr_t right, std::uintptr_t& result) noexcept {
    if (right > (std::numeric_limits<std::uintptr_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

/**
 * Adds a signed self-relative offset without wrapping the address space.
 * @param base Address of the relative-offset field.
 * @param relative Signed byte offset from that field.
 * @param result Receives the address found.
 * @return True when the signed sum fits.
 */
[[nodiscard]] inline bool
add_signed_fits(std::uintptr_t base, std::int64_t relative, std::uintptr_t& result) noexcept {
    if (relative >= 0) {
        const auto positive = static_cast<std::uint64_t>(relative);
        if (positive > (std::numeric_limits<std::uintptr_t>::max)()) {
            return false;
        }
        return add_fits(base, static_cast<std::uintptr_t>(positive), result);
    }

    // Adding one before negation keeps the most-negative signed value representable.
    const std::uint64_t magnitude = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
    if (magnitude > (std::numeric_limits<std::uintptr_t>::max)()
        || static_cast<std::uintptr_t>(magnitude) > base) {
        return false;
    }
    result = base - static_cast<std::uintptr_t>(magnitude);
    return true;
}

/**
 * Multiplies two address parts without wrapping the address space.
 * @param result Receives the whole product.
 * @return True when the product fits.
 */
[[nodiscard]] inline bool
multiply_fits(std::uintptr_t left, std::uintptr_t right, std::uintptr_t& result) noexcept {
    if (left != 0 && right > (std::numeric_limits<std::uintptr_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

/**
 * Reads one whole scalar through the caller's bounded memory source.
 * @tparam Value Trivially copied scalar or layout object.
 * @param source Installed item-detail source.
 * @param value Receives the whole value.
 * @return True when the callback reads every byte.
 */
template <typename Value>
[[nodiscard]] bool
read_value(const investment::Source& source, std::uintptr_t address, Value& value) noexcept {
    return source.handles.read != nullptr
           && source.handles.read(source.handles.context,
                                  address,
                                  std::span(reinterpret_cast<std::byte*>(&value), sizeof value));
}

/**
 * Works out a checked address for one typed member of a loaded blob.
 * @param base Loaded blob address.
 * @param offset Byte offset of the member.
 * @param address Receives the member address.
 * @return True when the member address fits.
 */
[[nodiscard]] inline bool
member_address(std::uintptr_t base, std::size_t offset, std::uintptr_t& address) noexcept {
    return add_fits(base, static_cast<std::uintptr_t>(offset), address);
}

/**
 * Reads one typed member at a checked offset from a loaded blob.
 * @tparam Value Trivially copied member type.
 * @param source Installed item-detail source.
 * @param base Loaded blob address.
 * @param offset Byte offset of the member.
 * @param value Receives the whole member.
 * @return True when the address fits and the read succeeds.
 */
template <typename Value>
[[nodiscard]] bool read_member(const investment::Source& source,
                               std::uintptr_t base,
                               std::size_t offset,
                               Value& value) noexcept {
    std::uintptr_t address = 0;
    return member_address(base, offset, address) && read_value(source, address, value);
}

/**
 * Reads one handle member and finds the blob it points to.
 * @param source Installed item-detail source.
 * @param parent Loaded parent blob address.
 * @param offset Byte offset of the handle member.
 * @param child Receives the loaded child blob address.
 * @return True when the member reads and its loaded handle is found.
 */
[[nodiscard]] inline bool resolve_member(const investment::Source& source,
                                         std::uintptr_t parent,
                                         std::size_t offset,
                                         std::uintptr_t& child) noexcept {
    std::uint32_t handle = 0;
    return read_member(source, parent, offset, handle)
           && handles::resolve(source.handles, handle, child);
}

} // namespace sunrise::client::content::items::details::memory
