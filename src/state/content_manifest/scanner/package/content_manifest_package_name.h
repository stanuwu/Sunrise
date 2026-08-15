#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../definition.h"

namespace sunrise::state::content_manifest::scanner::package {

/** Result of classifying one package-directory entry. */
enum class NameStatus : std::uint8_t {
    unrecognized,
    excluded,
    invalid,
    recognized,
};

/** Parsed public fields from one patchable package filename. */
struct ParsedName final {
    std::array<char, kPackageNameCapacity> stem{};
    std::uint16_t stemLength{};
    std::uint16_t identityLength{};
    std::uint16_t packageId{};
    std::uint32_t patchIndex{};
};

/**
 * Classifies one Windows package-directory filename and normalizes it.
 * @param fileName One leaf name including its extension.
 * @param output Cleared parsed fields for recognized names.
 * @return Recognized, excluded, unrecognized, or invalid.
 */
[[nodiscard]] NameStatus parse_file_name(std::wstring_view fileName, ParsedName& output) noexcept;

/**
 * Checks one canonical cached package stem, with no extension.
 * @param stem Lowercase ASCII package stem.
 * @param output Cleared parsed fields for a valid patchable stem.
 * @return True when the stem has the exact recognized form.
 */
[[nodiscard]] bool parse_stem(std::string_view stem, ParsedName& output) noexcept;

} // namespace sunrise::state::content_manifest::scanner::package
