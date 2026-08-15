#include "registry.h"

namespace sunrise::client::patterns {
namespace {

/**
 * Validates one pattern before the shared image sweep.
 * @param pattern Pattern name, bytes, and exact-byte mask.
 * @return True when the pattern has a name, bytes, and at least one exact byte.
 */
[[nodiscard]] bool is_valid(const Pattern& pattern) noexcept {
    if (pattern.name.empty() || pattern.bytes.empty()) {
        return false;
    }
    for (const PatternByte byte : pattern.bytes) {
        if (byte.exact) {
            return true;
        }
    }
    return false;
}

/**
 * Tests one pattern at one bounded image offset.
 * @param image Executable image bytes.
 * @param pattern Masked pattern bytes.
 * @return True when every exact byte matches.
 */
[[nodiscard]] bool matches_at(std::span<const std::byte> image,
                              std::size_t offset,
                              std::span<const PatternByte> pattern) noexcept {
    if (pattern.size() > image.size() - offset) {
        return false;
    }
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index].exact && image[offset + index] != pattern[index].value) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Resolves all patterns during one sweep over one executable range. */
bool resolve_all(std::span<std::byte> image,
                 std::span<const Pattern> patterns,
                 std::span<Match> matches) noexcept {
    const ImageRange range{image};
    return resolve_all(std::span(&range, 1), patterns, matches);
}

/** Resolves all patterns during one sweep over disjoint executable ranges. */
bool resolve_all(std::span<const ImageRange> image,
                 std::span<const Pattern> patterns,
                 std::span<Match> matches) noexcept {
    if (patterns.size() != matches.size()) {
        return false;
    }

    for (std::size_t index = 0; index < patterns.size(); ++index) {
        matches[index] = is_valid(patterns[index]) ? Match{MatchStatus::missing, nullptr} : Match{};
    }

    for (const ImageRange range : image) {
        for (std::size_t offset = 0; offset < range.bytes.size(); ++offset) {
            for (std::size_t index = 0; index < patterns.size(); ++index) {
                Match& match = matches[index];
                if (match.status == MatchStatus::invalid
                    || match.status == MatchStatus::ambiguous) {
                    continue;
                }
                if (!matches_at(range.bytes, offset, patterns[index].bytes)) {
                    continue;
                }

                if (match.status == MatchStatus::missing) {
                    match = Match{MatchStatus::unique, range.bytes.data() + offset};
                } else {
                    // A second match invalidates the address instead of choosing one.
                    match = Match{MatchStatus::ambiguous, nullptr};
                }
            }
        }
    }
    return true;
}

/** Collects bounded matches for one signature that is expected to repeat. */
std::size_t collect_matches(std::span<const ImageRange> image,
                            const Pattern& pattern,
                            std::span<std::byte*> output) noexcept {
    if (!is_valid(pattern) || output.empty()) {
        return 0;
    }
    std::size_t count = 0;
    for (const ImageRange range : image) {
        for (std::size_t offset = 0; offset < range.bytes.size(); ++offset) {
            if (!matches_at(range.bytes, offset, pattern.bytes)) {
                continue;
            }
            output[count++] = range.bytes.data() + offset;
            if (count == output.size()) {
                return count;
            }
        }
    }
    return count;
}

} // namespace sunrise::client::patterns
