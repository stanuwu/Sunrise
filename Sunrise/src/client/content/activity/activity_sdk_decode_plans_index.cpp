#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {
namespace {

/** Codec block: serialized size at +0x18, field rows from +0x28. */
constexpr std::uint64_t kCodecSerializedAt = 0x18;
constexpr std::uint64_t kCodecFieldsAt = 0x28;
/** Bit offset of one field inside its 40-byte row. */
constexpr std::uint64_t kFieldBitAt = 8;

} // namespace

/**
 * Walks every bound record's block chain and rows its codec block.
 * The chain must close exactly and a codec block must name its own bound handle.
 */
bool build_codec_index(const Image& image,
                       std::vector<SchemaRow>& rows,
                       std::vector<std::uint32_t>& fieldValues,
                       const char*& error) noexcept {
    try {
        rows.clear();
        fieldValues.clear();
        for (const auto& [handle, address] : image.slots()) {
            std::uint32_t length = 0;
            if (!image.u32(address, length)) {
                error = "record_read";
                return false;
            }
            std::uint32_t marker = 0;
            for (const std::uint32_t candidate : kMarkerOffsets) {
                std::uint32_t value = 0;
                if (candidate + 8 <= length && image.u32(address + candidate, value)
                    && value == kReflectionMarker) {
                    marker = candidate;
                    break;
                }
            }
            if (marker == 0) {
                continue;
            }
            std::uint64_t cursor = marker;
            bool codec = false;
            std::uint64_t codecAt = 0;
            std::uint32_t codecCount = 0;
            while (cursor < length) {
                std::uint32_t kind = 0;
                std::uint32_t count = 0;
                BlockLayout layout{};
                if (!image.u32(address + cursor + 4, kind)
                    || !image.u32(address + cursor + 8, count)) {
                    error = "record_read";
                    return false;
                }
                if (!block_layout(kind, layout)) {
                    error = "reflection_block_unknown";
                    return false;
                }
                const std::uint64_t size = layout.header + std::uint64_t{layout.stride} * count;
                if (cursor + size > length) {
                    error = "reflection_block_extent";
                    return false;
                }
                if (kind == kCodecBlock) {
                    std::uint32_t bound = 0;
                    if (!image.u32(address + cursor + kCodecHandleAt, bound)) {
                        error = "record_read";
                        return false;
                    }
                    if (bound != handle) {
                        error = "codec_handle_mismatch";
                        return false;
                    }
                    if (codec) {
                        error = "codec_block_repeated";
                        return false;
                    }
                    codec = true;
                    codecAt = cursor;
                    codecCount = count;
                }
                cursor += size;
            }
            if (cursor != length) {
                error = "reflection_chain_open";
                return false;
            }
            if (!codec) {
                continue;
            }
            SchemaRow row{handle, 0, static_cast<std::uint32_t>(fieldValues.size()), codecCount};
            if (!image.u32(address + codecAt + kCodecSerializedAt, row.size)) {
                error = "record_read";
                return false;
            }
            for (std::uint32_t index = 0; index < codecCount; ++index) {
                std::uint32_t bit = 0;
                if (!image.u32(address + codecAt + kCodecFieldsAt + kFieldRowSize * index
                                   + kFieldBitAt,
                               bit)) {
                    error = "record_read";
                    return false;
                }
                fieldValues.push_back(bit);
            }
            rows.push_back(row);
        }
        return true;
    } catch (...) {
        error = "allocation";
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal
