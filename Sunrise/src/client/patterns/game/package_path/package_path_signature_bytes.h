#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../registry.h"
#include "../../signature_text.h"

namespace sunrise::client::patterns::game::package_path {

/**
 * Builds a package file object's path from its manifest name: `packages\<name>.pkg`, or the name
 * as given when it already holds `.pkg`. The path field offset and its capacity are operands.
 */
inline constexpr std::string_view kSetPathText =
    "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 48 8B CB 48 8D 15 ? ? ? ? E8 ? ? ? ? 48 8D 8F "
    "? ? ? ? 48 85 C0 74 ? 41 B8 ? ? ? ? 48 8B D3 48 8B 5C 24 ? 48 83 C4 ? 5F E9 ? ? ? ? 4C 8B C3 "
    "48 8D 15";
/** Byte offset of the path field displacement in `lea rcx, [rdi + disp32]`. */
inline constexpr std::size_t kPathOffsetOperand = 34;
/** Byte offset of the path capacity immediate in `mov r8d, imm32`. */
inline constexpr std::size_t kPathCapacityOperand = 45;

/** Opens a stdio file from a path object: resolve its root and text, then `_wfsopen`. */
inline constexpr std::string_view kOpenFileText =
    "48 89 5C 24 ? 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 41 8B F8 "
    "8B DA 41 B8 ? ? ? ? 48 8D 54 24 ? E8 ? ? ? ? 44 8B C7 48 8D 4C 24 ? 8B D3 E8";

/** Turns a path object into a full path; root 11 goes through the known-file lookup. */
inline constexpr std::string_view kResolvePathText =
    "40 53 55 56 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 83 B9 ? ? ? ? "
    "0B 48 8B F2 49 63 E8 48 8B D9 0F 85 ? ? ? ? 8B 81 ? ? ? ? C1 E8 02 A8 01 0F 85";
/** Byte offset of the root id displacement in `cmp dword ptr [rcx + disp32], 11`. */
inline constexpr std::size_t kRootOffsetOperand = 31;
/** Byte offset of the path flags displacement in `mov eax, [rcx + disp32]`. */
inline constexpr std::size_t kFlagsOffsetOperand = 53;

/**
 * Runs one schema callback of a loading tag: slot, tag, blob. Index 2, the fixup, is the first
 * callback after the bytes land, and it runs for every tag a load request brings in.
 */
inline constexpr std::string_view kSchemaCallbackText =
    "4C 8B DC 49 89 5B ? 49 89 6B ? 49 89 73 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 4C 8B BC 24 "
    "? ? ? ? 4D 8D 71 ? 41 8B 1E 33 C0 45 32 ED 49 89 43 ? C6 44 24 ? 01 41 BC 01 00 00 00 49 89 "
    "43 ? 41 8B F8 49 89 43 ? 8B EA 45 89 63 ? 88 84 24 ? ? ? ? 4D 85 FF 74 ? 49 89 07 4C 8D 44 "
    "24 ? 8B D3 49 8B C9 E8";

/** Package path builder signature byte count. */
inline constexpr std::size_t kSetPathPatternSize = signature_length(kSetPathText);
/** Path object file opener signature byte count. */
inline constexpr std::size_t kOpenFilePatternSize = signature_length(kOpenFileText);
/** Path resolver signature byte count. */
inline constexpr std::size_t kResolvePathPatternSize = signature_length(kResolvePathText);
/** Schema callback dispatcher signature byte count. */
inline constexpr std::size_t kSchemaCallbackPatternSize = signature_length(kSchemaCallbackText);

extern constinit const std::array<patterns::PatternByte, kSetPathPatternSize> kSetPath;
extern constinit const std::array<patterns::PatternByte, kOpenFilePatternSize> kOpenFile;
extern constinit const std::array<patterns::PatternByte, kResolvePathPatternSize> kResolvePath;
extern constinit const std::array<patterns::PatternByte, kSchemaCallbackPatternSize>
    kSchemaCallback;

} // namespace sunrise::client::patterns::game::package_path
