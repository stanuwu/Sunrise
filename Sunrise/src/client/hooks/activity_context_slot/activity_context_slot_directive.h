#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::client::hooks::activity_context_slot {

/** What one mission script asks for the replicated-record slot of its activity context. */
enum class Request : std::uint8_t {
    /** The script says nothing, so the Client keeps its own slot. */
    none,
    /** The script asks for the Client's own slot. */
    native,
    /** The script asks for slot 1, the value the Client's activity-swap path passes. */
    swapped,
    /** The directive is malformed or repeated. Treated as `none`, and reported. */
    invalid,
};

/**
 * Reads the `sunrise.activity_context_slot` directive from a script's leading line comments:
 *
 *     --! sunrise.activity_context_slot = "swapped"
 *
 * Only the header counts: the scan stops at the first line that is neither blank nor a `--` line
 * comment, and at the first block comment. Other `--!` lines are skipped. The value is `swapped`
 * or `native`, quoted or bare.
 * @param header The start of the script.
 * @param wholeFile True when header is the whole script. When false, an unterminated last line is
 * ignored, because it may continue past the bytes that were read.
 * @return The request, or `invalid` for a malformed or repeated directive.
 */
[[nodiscard]] Request read_directive(std::string_view header, bool wholeFile) noexcept;

/** @return Stable log text for one request. */
[[nodiscard]] const char* request_name(Request value) noexcept;

} // namespace sunrise::client::hooks::activity_context_slot
