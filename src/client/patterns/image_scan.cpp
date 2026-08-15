#include "image_scan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "../executable/image.h"
#include "registry.h"

namespace sunrise::client::patterns {

/** Scans the main image for one signature, independently of the shared registry. */
std::byte* scan_main_image_unique(std::span<const PatternByte> signature,
                                  const char* name) noexcept {
    executable::ExecutableImage main{};
    if (!executable::inspect_main_module(main)) {
        return nullptr;
    }
    std::array<ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t index = 0; index < main.count; ++index) {
        ranges[index] = ImageRange{main.sections[index]};
    }
    const std::array definitions{Pattern{name, signature}};
    std::array<Match, 1> matches{};
    if (!resolve_all(std::span(ranges.data(), main.count), definitions, matches)
        || matches[0].status != MatchStatus::unique) {
        return nullptr;
    }
    return matches[0].address;
}

/** Decodes a RIP-relative operand into the absolute address it refers to. */
std::byte* resolve_relative(const std::byte* operand, const std::byte* nextInstruction) noexcept {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, operand, sizeof displacement);
    return const_cast<std::byte*>(nextInstruction) + displacement;
}

} // namespace sunrise::client::patterns
