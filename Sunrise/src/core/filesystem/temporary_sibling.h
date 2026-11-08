#pragma once

#include <string_view>

#include "path.h"

namespace sunrise::core::path {

/**
 * Builds a writer-owned sibling name: final.process.thread.sequence.tmp.
 * @param finalPath Final file path.
 * @param output Receives the sibling path.
 * @return False when the name does not fit.
 */
[[nodiscard]] bool temporary_sibling(std::wstring_view finalPath, Buffer& output) noexcept;

/**
 * Removes bounded, old writer-owned temporary siblings whose process has stopped.
 * A sibling is named final.process.thread.sequence.tmp, so a crashed writer leaves one behind.
 * @param finalPath Null-terminated final file path that owns the sibling names.
 */
void remove_stale_siblings(const wchar_t* finalPath) noexcept;

/**
 * Publishes one complete sibling while open delete-sharing readers keep the old file.
 * @param temporaryPath Existing writer-owned sibling path.
 * @param finalPath Final path to create or replace.
 * @return True when the sibling became the complete final file.
 */
[[nodiscard]] bool publish_sibling(const wchar_t* temporaryPath, const wchar_t* finalPath) noexcept;

/**
 * Writes `bytes` to a sibling, flushes it, then publishes it as `finalPath`.
 * @return True when `bytes` is the complete file on disk.
 */
[[nodiscard]] bool replace_file(std::wstring_view finalPath, std::string_view bytes) noexcept;

} // namespace sunrise::core::path
