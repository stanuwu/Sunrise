#include "socket_menu_native_array.h"

#include <Windows.h>

#include <cstring>
#include <limits>

#include "../../../memory/current_process_memory.h"

namespace sunrise::client::hooks::network::investment::native_array {

void* image_pointer(std::uintptr_t address) noexcept {
    void* pointer = nullptr;
    static_assert(sizeof pointer == sizeof address);
    std::memcpy(&pointer, &address, sizeof pointer);
    return pointer;
}

bool read_bytes(std::uintptr_t address, std::span<std::byte> output) noexcept {
    return !output.empty() && memory::read_current_process(nullptr, address, output);
}

/**
 * Writes bytes over read-only image data, then puts the page protection back.
 * @return False when the address is null, the span is empty, or the page cannot be opened.
 */
bool write_bytes(std::uintptr_t address, std::span<const std::byte> bytes) noexcept {
    if (address == 0 || bytes.empty()) {
        return false;
    }
    void* destination = image_pointer(address);
    DWORD previous = 0;
    if (VirtualProtect(destination, bytes.size(), PAGE_READWRITE, &previous) == FALSE) {
        return false;
    }
    std::memcpy(destination, bytes.data(), bytes.size());
    DWORD ignored = 0;
    const bool restored = VirtualProtect(destination, bytes.size(), previous, &ignored) != FALSE;
    return restored;
}

/**
 * Applies one signed self-relative displacement to a base address.
 * @param output Receives the result; unchanged on overflow.
 * @return False when the sum leaves the address space.
 */
bool add_relative(std::uintptr_t base, std::int64_t relative, std::uintptr_t& output) noexcept {
    if (relative >= 0) {
        const auto distance = static_cast<std::uint64_t>(relative);
        if (distance > (std::numeric_limits<std::uintptr_t>::max)() - base) {
            return false;
        }
        output = base + static_cast<std::uintptr_t>(distance);
        return true;
    }
    const auto distance = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
    if (distance > base) {
        return false;
    }
    output = base - static_cast<std::uintptr_t>(distance);
    return true;
}

/** Resolves one native count/self-relative array and validates its repeated header and class. */
bool resolve_array(std::uintptr_t descriptor,
                   std::size_t maximumCount,
                   std::size_t stride,
                   std::uint32_t expectedClass,
                   ArrayView& output) noexcept {
    output = {};
    ArrayDescriptor encoded{};
    if (!read(descriptor, encoded) || encoded.count <= 0
        || static_cast<std::uint64_t>(encoded.count) > maximumCount) {
        return false;
    }
    const auto count = static_cast<std::uint64_t>(encoded.count);
    std::uintptr_t header = 0;
    if (!add_relative(descriptor
                          + middleware::content::packages::reader::layout::kArrayFieldRelative,
                      encoded.relative,
                      header)
        || header < kArrayMarkerSize) {
        return false;
    }
    std::uint32_t marker = 0;
    std::uint64_t repeatedCount = 0;
    std::uint32_t elementClass = 0;
    if (!read(header - kArrayMarkerSize, marker)
        || !read(header + offsetof(relocation::ArrayHeader, count), repeatedCount)
        || !read(header + offsetof(relocation::ArrayHeader, elementClass), elementClass)
        || repeatedCount != count || marker != relocation::kMarker || elementClass != expectedClass
        || count > ((std::numeric_limits<std::uintptr_t>::max)() - header - kArrayHeaderSize)
                       / stride) {
        return false;
    }
    const std::uintptr_t data = header + kArrayHeaderSize;
    std::byte tail{};
    if (!read(data + static_cast<std::uintptr_t>(count * stride) - 1U, tail)) {
        return false;
    }
    output = {descriptor, header, data, count, elementClass};
    return true;
}

} // namespace sunrise::client::hooks::network::investment::native_array
