#include "handle_resolver.h"

#include <limits>

#include "layout.h"

namespace sunrise::client::content::handles {
namespace {

/** The 13 low handle bits pick one record within its table. */
constexpr std::uint32_t kRecordIndexBits = 13;
/** The record-index mask covers all 13 encoded low bits. */
constexpr std::uint32_t kRecordIndexMask = (1U << kRecordIndexBits) - 1U;
/** The top handle bit picks the extended 14-bit datum-table namespace. */
constexpr std::uint32_t kHandleSignBit = 1U << ((std::numeric_limits<std::uint32_t>::digits) - 1U);
/** Explicit sign-extension bits copy the datum selector's arithmetic shift. */
constexpr std::uint32_t kSignedHighFill =
    ~((std::numeric_limits<std::uint32_t>::max)() >> kRecordIndexBits);
/** The encoded table sum fills the fixed high guard bits before shifting. */
constexpr std::uint64_t kTableMaskFill = 0x0FFC0000ULL;
/** The datum selector stores its table-width mask above bit 17. */
constexpr unsigned kTableMaskShift = 18;
/** High-bit datum handles hold a 14-bit descriptor-table id. */
constexpr unsigned kHighBitTableIndexBits = 14;
/** Content and schema datum tables are registered from descriptor id 1024. */
constexpr std::uint64_t kMinimumContentTableIndex = 1024;
/** 14 selector bits bound the process handle-table descriptor array. */
constexpr std::uint64_t kMaximumTableIndex = (1ULL << kHighBitTableIndexBits) - 1ULL;
/** All-one handle bits mean the loaded content is not there. */
constexpr std::uint32_t kUnavailableContentHandle = (std::numeric_limits<std::uint32_t>::max)();

/**
 * Adds two address parts without wrapping the address space.
 * @param left Base address.
 * @param right Byte offset.
 * @param result Receives the whole address.
 * @return True when the sum fits.
 */
[[nodiscard]] bool
add_fits(std::uintptr_t left, std::uintptr_t right, std::uintptr_t& result) noexcept {
    if (right > (std::numeric_limits<std::uintptr_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

/**
 * Multiplies two address parts without wrapping.
 * @param result Receives the product.
 * @return True when the product fits.
 */
[[nodiscard]] bool
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
 * @param value Receives the whole value.
 * @return True when the callback reads every byte.
 */
template <typename Value>
[[nodiscard]] bool read_value(const Source& source, std::uintptr_t address, Value& value) noexcept {
    return source.read != nullptr
           && source.read(source.context,
                          address,
                          std::span(reinterpret_cast<std::byte*>(&value), sizeof value));
}

} // namespace

/** Finds one loaded content handle without holding on to a game pointer. */
bool resolve(const Source& source, std::uint32_t handle, std::uintptr_t& address) noexcept {
    address = 0;
    if (source.tablesSlot == 0 || source.read == nullptr || handle == kUnavailableContentHandle) {
        return false;
    }

    std::uintptr_t tablesObject = 0;
    std::uintptr_t tableBase = 0;
    if (!read_value(source, source.tablesSlot, tablesObject) || tablesObject == 0
        || !read_value(source, tablesObject, tableBase) || tableBase == 0) {
        return false;
    }

    // Explicit sign extension keeps content table 1024 from aliasing generic table zero.
    const std::uint32_t encodedHigh =
        (handle >> kRecordIndexBits) | (((handle & kHandleSignBit) != 0) ? kSignedHighFill : 0U);
    const std::uint64_t tableMask =
        (static_cast<std::uint64_t>(encodedHigh) | kTableMaskFill) >> kTableMaskShift;
    const std::uint64_t tableIndex = static_cast<std::uint16_t>(encodedHigh) & tableMask;
    if (tableIndex < kMinimumContentTableIndex || tableIndex > kMaximumTableIndex) {
        return false;
    }

    std::uintptr_t tableOffset = 0;
    std::uintptr_t tableAddress = 0;
    if (!multiply_fits(
            static_cast<std::uintptr_t>(tableIndex), sizeof(layout::TableDescriptor), tableOffset)
        || !add_fits(tableBase, tableOffset, tableAddress)) {
        return false;
    }

    layout::TableDescriptor table{};
    if (!read_value(source, tableAddress, table) || table.recordArray == 0
        || table.recordStride == 0) {
        return false;
    }

    const std::uint32_t recordIndex = handle & kRecordIndexMask;
    std::uintptr_t recordOffset = 0;
    std::uintptr_t recordAddress = 0;
    layout::RecordPrefix record{};
    // The record offset is a 32-bit ABI product, so an overflowing one is rejected.
    if (!multiply_fits(recordIndex, table.recordStride, recordOffset)
        || recordOffset > (std::numeric_limits<std::uint32_t>::max)()
        || !add_fits(table.recordArray, recordOffset, recordAddress)
        || !read_value(source, recordAddress, record)) {
        return false;
    }

    const std::uint64_t extendedMask =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(table.correctionMask));
    const std::uint64_t correction = record.correctionSource & extendedMask;
    if (correction > recordAddress) {
        return false;
    }
    address = recordAddress - static_cast<std::uintptr_t>(correction);
    return address != 0;
}

} // namespace sunrise::client::content::handles
