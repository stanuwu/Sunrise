#include "account_selection_patch_encoder.h"

#include <algorithm>
#include <array>

#include "../../../../encoding/bit_writer.h"

namespace sunrise::middleware::datagen::family4::account::selection_patch {
namespace {

/** 4 optional fields come before the selected-character field. */
constexpr std::uint8_t kLeadingAbsentWidth = 4;
/** The selected-character field carries an explicit presence bit. */
constexpr std::uint8_t kPresenceWidth = 1;
/** The selected-character id is one full unsigned 64-bit field. */
constexpr std::uint8_t kSelectedSoidWidth = 64;
/** 23 optional fields separate selection from the generated scalar. */
constexpr std::uint8_t kMiddleAbsentWidth = 23;
/** The generated scalar is mandatory even when its value is 0. */
constexpr std::uint8_t kGeneratedScalarWidth = 32;
/** 5 optional fields follow the generated scalar. */
constexpr std::uint8_t kTrailingAbsentWidth = 5;

} // namespace

/** Encodes only the selected-character SOID and its mandatory generated 0. */
bool encode(std::uint64_t selectedSoid,
            std::span<std::byte> output,
            std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kPayloadSize) {
        return false;
    }

    std::array<std::byte, kPayloadSize> staged{};
    encoding::bits::Writer writer(staged);
    const bool encoded =
        writer.write(0, kLeadingAbsentWidth) && writer.write(1, kPresenceWidth)
        && writer.write(selectedSoid, kSelectedSoidWidth) && writer.write(0, kMiddleAbsentWidth)
        && writer.write(0, kGeneratedScalarWidth) && writer.write(0, kTrailingAbsentWidth);
    std::size_t stagedSize = 0;
    if (!encoded || writer.bit_count() != kPayloadBitSize || !writer.finish(stagedSize)
        || stagedSize != staged.size()) {
        return false;
    }

    std::copy(staged.begin(), staged.end(), output.begin());
    written = staged.size();
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account::selection_patch
