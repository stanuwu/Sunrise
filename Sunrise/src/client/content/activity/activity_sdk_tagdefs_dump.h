#pragma once

#include <cstdint>
#include <string_view>

#include "activity_sdk_decode_plans_internal.h"

// Writes the engine's tag reflection records as raw JSON, the input of the Studio registry.
namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {

/** Counts from one complete dump. */
struct DumpResult final {
    std::uint32_t slots{};
    std::uint32_t native{};
    std::uint32_t nativeErrors{};
    std::uint32_t package{};
    std::uint32_t packageErrors{};
};

/**
 * Writes every bound slot record and, with a source, every package metadata record.
 * @param image Opened executable; its file digest is the build key.
 * @param source Package source with block keys, or null to dump the executable only.
 * @param outputDirectory Existing directory; receives tagdefs-raw.<sha8>.json.
 * @param output Receives the record counts.
 * @return Status::ready when the file is complete on disk.
 */
[[nodiscard]] Status dump_tagdefs(const Image& image,
                                  const reader::Source* source,
                                  std::wstring_view outputDirectory,
                                  DumpResult& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal

namespace sunrise::client::content::activity::sdk_generation::decode_plans {

/** Starts one tagdefs export on its own thread when `client.exports.tag_definitions` is on. */
void export_tagdefs_if_requested() noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans
