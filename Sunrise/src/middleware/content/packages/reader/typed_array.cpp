#include "typed_array.h"

#include <cstring>

namespace sunrise::middleware::content::packages::reader {
namespace {

/** @return The little-endian word at `offset`; the caller has bounds-checked it. */
template <typename Value>
[[nodiscard]] Value word(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

} // namespace

/** Checks one typed-array header and finds its rows. */
bool read_typed_array(std::span<const std::byte> bytes,
                      std::size_t header,
                      std::uint64_t count,
                      std::uint32_t elementClass,
                      std::size_t stride,
                      TypedArrayRows& rows) noexcept {
    using Header = layout::TypedArrayHeader;
    // Subtracting from the size keeps a large header offset from wrapping.
    if (header < layout::kClassWordSize || bytes.size() < Header::kSize
        || header > bytes.size() - Header::kSize) {
        return false;
    }
    const auto actualClass = word<std::uint32_t>(bytes, header + Header::kClass);
    if (word<std::uint32_t>(bytes, header - layout::kClassWordSize) != layout::kTableArrayMarker
        || word<std::uint64_t>(bytes, header + Header::kCount) != count
        || (elementClass != kAnyElementClass && actualClass != elementClass)) {
        return false;
    }
    const std::size_t data = header + Header::kSize;
    if (stride != 0 && count > (bytes.size() - data) / stride) {
        return false;
    }
    rows = TypedArrayRows{data, actualClass};
    return true;
}

} // namespace sunrise::middleware::content::packages::reader
