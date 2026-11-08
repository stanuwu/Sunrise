#include "activity_sdk_dialogue_group_index.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "../../../middleware/content/packages/schema/generated/tag_classes_80808d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "../../../state/activity_sdk/format.h"

namespace sunrise::client::content::activity::sdk_generation::dialogue_group_index {
namespace {

namespace generated = middleware::content::packages::schema::generated;

// The bank stores a counted definition array; its rows follow the array header.
constexpr std::size_t kDefinitionsField = offsetof(generated::DialogueAuthoredList, events);
constexpr std::size_t kDefinitionsRelative =
    offsetof(generated::DialogueAuthoredList, events.relative);
constexpr std::size_t kDefinitionStride = sizeof(generated::DialogueDefinition);
constexpr std::size_t kArrayDataOffset = sizeof(generated::STypedArrayMarker);
// An empty authored name uses the FNV-1 basis.
constexpr std::uint32_t kAbsentDefinitionHash = 0x811C9DC5U;

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> bytes, std::size_t offset, Value& output) noexcept {
    output = {};
    if (offset > bytes.size() || sizeof output > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof output);
    return true;
}

/** Applies one signed blob-relative offset. @return False when the result leaves the blob. */
[[nodiscard]] bool
add_relative(std::size_t member, std::int64_t relative, std::size_t& target) noexcept {
    if (relative >= 0) {
        const auto distance = static_cast<std::uint64_t>(relative);
        if (distance > (std::numeric_limits<std::size_t>::max)() - member) {
            return false;
        }
        target = member + static_cast<std::size_t>(distance);
        return true;
    }
    const auto distance = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
    if (distance > member) {
        return false;
    }
    target = member - static_cast<std::size_t>(distance);
    return true;
}

} // namespace

/**
 * Reads cue windows in bank order.
 * @param bytes Authored dialogue bank.
 * @param output Receives every row; cleared on failure.
 * @return False for an invalid window or array.
 */
bool definitions(std::span<const std::byte> bytes, std::vector<Definition>& output) noexcept {
    namespace format = state::activity_sdk::format;
    output.clear();
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    std::size_t header = 0;
    std::uint64_t repeated = 0;
    std::uint32_t rowClass = 0;
    if (!read_value(bytes, kDefinitionsField, count) || count > format::kDialogueMaximumCueCount
        || !read_value(bytes, kDefinitionsRelative, relative)
        || !add_relative(kDefinitionsRelative, relative, header)
        || !read_value(bytes, header, repeated) || repeated != count
        || !read_value(
            bytes, header + offsetof(generated::STypedArrayMarker, elementClass), rowClass)
        || rowClass != generated::DialogueDefinition::kClass || header > bytes.size()
        || kArrayDataOffset > bytes.size() - header
        || count > (bytes.size() - header - kArrayDataOffset) / kDefinitionStride) {
        return false;
    }
    try {
        output.reserve(static_cast<std::size_t>(count));
        for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
            const std::size_t offset = header + kArrayDataOffset + ordinal * kDefinitionStride;
            Definition row{};
            if (!read_value(bytes, offset + offsetof(generated::DialogueDefinition, hash), row.hash)
                || !read_value(bytes,
                               offset
                                   + offsetof(generated::DialogueDefinition, authoredWindowSeconds),
                               row.authoredWindowSeconds)
                || row.hash == 0 || row.hash == kAbsentDefinitionHash
                || !std::isfinite(row.authoredWindowSeconds) || row.authoredWindowSeconds < 0.0F) {
                output.clear();
                return false;
            }
            output.push_back(row);
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::dialogue_group_index
