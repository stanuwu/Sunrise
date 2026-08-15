#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace sunrise::client::patterns {

/** One exact or wildcard byte in a compiled signature. */
struct PatternByte {
    std::byte value{};
    bool exact{};
};

/** Named byte signature resolved against executable image ranges. */
struct Pattern {
    std::string_view name;
    std::span<const PatternByte> bytes;
};

/** How many times one signature matched. */
enum class MatchStatus : unsigned char {
    invalid,
    missing,
    unique,
    ambiguous,
};

/** Resolution status and unique address for one signature. */
struct Match {
    MatchStatus status{MatchStatus::invalid};
    std::byte* address{};
};

/** One mapped executable range eligible for signature scans. */
struct ImageRange {
    std::span<std::byte> bytes;
};

/** Resolves every registered pattern during one image sweep. */
[[nodiscard]] bool resolve_all(std::span<std::byte> image,
                               std::span<const Pattern> patterns,
                               std::span<Match> matches) noexcept;

/** Resolves every pattern across disjoint executable image ranges. */
[[nodiscard]] bool resolve_all(std::span<const ImageRange> image,
                               std::span<const Pattern> patterns,
                               std::span<Match> matches) noexcept;

/**
 * Collects bounded matches for one signature that is expected to repeat.
 * Use this only where several matches are the evidence; single targets use resolve_all.
 * @param image Executable ranges to scan in address order.
 * @param output Fixed storage receiving match addresses in address order.
 * @return Number of addresses written, capped at the output size.
 */
[[nodiscard]] std::size_t collect_matches(std::span<const ImageRange> image,
                                          const Pattern& pattern,
                                          std::span<std::byte*> output) noexcept;

} // namespace sunrise::client::patterns
