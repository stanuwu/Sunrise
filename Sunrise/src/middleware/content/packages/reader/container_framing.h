#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Words that frame records inside tag bytes. No tag class layout lives here.
namespace sunrise::middleware::content::packages::reader::layout {

/** A u32 class id word precedes every inline struct and typed-array header. */
inline constexpr std::size_t kClassWordSize = sizeof(std::uint32_t);
/** An inline record starts with its class word, then a zero word, then the record. */
inline constexpr std::size_t kInlineRecordHeaderSize = kClassWordSize + sizeof(std::uint32_t);
/** A root tag may open with a u64 that holds its own byte length. */
inline constexpr std::size_t kSizeWordSize = sizeof(std::uint64_t);

/** @return True when the tag's first u64 holds its own length. */
[[nodiscard]] inline bool has_size_word(std::span<const std::byte> tag) noexcept {
    std::uint64_t size = 0;
    if (tag.size() < kSizeWordSize) {
        return false;
    }
    std::memcpy(&size, tag.data(), sizeof size);
    return size == tag.size();
}

/** Writes the tag's length into its first u64; a tag too short for the word is left as it is. */
inline void write_size_word(std::span<std::byte> tag) noexcept {
    const std::uint64_t size = tag.size();
    if (tag.size() >= kSizeWordSize) {
        std::memcpy(tag.data(), &size, sizeof size);
    }
}

/** A rewritten tag keeps a size word only when its source had one. */
inline void carry_size_word(std::span<const std::byte> source,
                            std::span<std::byte> output) noexcept {
    if (has_size_word(source)) {
        write_size_word(output);
    }
}

/**
 * Reads the class word just before the object at `offset`.
 * @return False when the word does not fit in `tag`; `classId` is then zero.
 */
[[nodiscard]] inline bool class_word_before(std::span<const std::byte> tag,
                                            std::size_t offset,
                                            std::uint32_t& classId) noexcept {
    classId = 0;
    if (offset < kClassWordSize || offset > tag.size()) {
        return false;
    }
    std::memcpy(&classId, tag.data() + offset - kClassWordSize, sizeof classId);
    return true;
}

} // namespace sunrise::middleware::content::packages::reader::layout
