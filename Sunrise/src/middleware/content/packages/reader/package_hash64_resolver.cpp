#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

/** The hash64 dynamic array begins at this offset inside the package misc-data object. */
constexpr std::uint64_t kHash64ArrayOffset = 0x30;
/** Tiger relative pointers resolve from their own field and include this authored bias. */
constexpr std::int64_t kRelativePointerBias = 0x10;
/** A corrupt package cannot make a diagnostic sweep unbounded. */
constexpr std::uint64_t kMaximumHash64Rows = 1U << 20;
/** Public rows are read in small fixed batches. */
constexpr std::size_t kBatchRows = 256;

struct ArrayHeader {
    std::uint64_t count{};
    std::int64_t relative{};
};

static_assert(sizeof(ArrayHeader) == 16);

template <typename Value>
[[nodiscard]] Value field(std::span<const std::byte, layout::kHeaderSize> bytes,
                          std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

} // namespace

/** Resolves one stable handle through the latest package's public hash64 table. */
bool resolve_hash64(std::wstring_view directory,
                    std::uint16_t packageId,
                    std::uint64_t hash64,
                    std::uint32_t& tag,
                    std::uint32_t& classId) noexcept {
    tag = 0;
    classId = 0;
    if (hash64 == 0) {
        return false;
    }

    Path stem{};
    Path path{};
    std::uint32_t patchIndex = 0;
    std::array<std::byte, layout::kHeaderSize> headerBytes{};
    Header header{};
    if (!find_latest(directory, packageId, stem, patchIndex)
        || !build_path(stem, patchIndex, path) || !read_at(path, 0, headerBytes)
        || !parse_header(headerBytes, header) || header.packageId != packageId
        || header.patchId != patchIndex) {
        return false;
    }

    const std::uint64_t miscData =
        field<std::uint32_t>(headerBytes, layout::HeaderOffsets::kMiscData);
    if (miscData == 0
        || miscData > (std::numeric_limits<std::uint64_t>::max)() - kHash64ArrayOffset) {
        return false;
    }
    const std::uint64_t arrayOffset = miscData + kHash64ArrayOffset;
    ArrayHeader array{};
    if (!read_at(path, arrayOffset, std::as_writable_bytes(std::span{&array, 1}))
        || array.count == 0 || array.count > kMaximumHash64Rows) {
        return false;
    }

    const std::uint64_t pointerField = arrayOffset + sizeof(std::uint64_t);
    if (pointerField > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const std::int64_t signedBase = static_cast<std::int64_t>(pointerField);
    if ((array.relative > 0
         && signedBase > (std::numeric_limits<std::int64_t>::max)() - array.relative)
        || (array.relative < 0
            && signedBase < (std::numeric_limits<std::int64_t>::min)() - array.relative)) {
        return false;
    }
    std::int64_t resolved = signedBase + array.relative;
    if (resolved > (std::numeric_limits<std::int64_t>::max)() - kRelativePointerBias) {
        return false;
    }
    resolved += kRelativePointerBias;
    if (resolved < 0) {
        return false;
    }

    std::array<layout::Hash64Record, kBatchRows> rows{};
    std::uint64_t index = 0;
    while (index < array.count) {
        const std::size_t count = static_cast<std::size_t>(
            (std::min)(array.count - index, static_cast<std::uint64_t>(rows.size())));
        const std::uint64_t byteOffset =
            static_cast<std::uint64_t>(resolved) + index * sizeof(layout::Hash64Record);
        if (!read_at(path,
                     byteOffset,
                     std::as_writable_bytes(std::span{rows.data(), count}))) {
            return false;
        }
        for (std::size_t position = 0; position < count; ++position) {
            if (rows[position].hash64 != hash64) {
                continue;
            }
            if (rows[position].hash32 < layout::kTagBase) {
                return false;
            }
            tag = rows[position].hash32;
            classId = rows[position].classId;
            return true;
        }
        index += count;
    }
    return false;
}

} // namespace sunrise::middleware::content::packages::reader
