#include "activity_sdk_dialogue_list.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "../../../middleware/content/packages/reader/layout.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_7f0007.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80808d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "../../../state/activity_sdk/format.h"
#include "activity_sdk_dialogue_group_index.h"

namespace sunrise::client::content::activity::sdk_generation::dialogue_list {
namespace {

namespace format = state::activity_sdk::format;
namespace generated = middleware::content::packages::schema::generated;
namespace layout = middleware::content::packages::reader::layout;
using ArrayHeader = generated::STypedArrayMarker;
using Sequence = generated::DialogueSequence;
using Selection = generated::DialogueSelection;
using SelectionEntry = generated::DialogueSelectionEntry;
using DialogueLine = generated::DialogueLine;
using TakeRecord = generated::G80808d2318;

// The cue definitions come in cue order; the content trees that follow are sorted by hash.
constexpr std::size_t kTreeField = offsetof(generated::DialogueAuthoredList, trees);
constexpr std::size_t kTreeStride = sizeof(generated::DialogueTreeRow);
// Every tree object repeats the definition hash it belongs to, after a per-object id.
constexpr std::size_t kObjectHashOffset = offsetof(Sequence, unk4);
static_assert(offsetof(Selection, unk4) == kObjectHashOffset
              && offsetof(DialogueLine, unk4) == kObjectHashOffset);
// A sequence plays its children in order through an array of relative object pointers.
constexpr std::uint32_t kSequenceClass = Sequence::kClass;
constexpr std::size_t kSequenceChildField = offsetof(Sequence, children);
constexpr std::uint32_t kPointerArrayClass = generated::DialoguePointer::kClass;
constexpr std::size_t kPointerStride = sizeof(generated::DialoguePointer);
// A selection plays one entry; each entry holds its conditions and a pointer to its child.
constexpr std::uint32_t kSelectionClass = Selection::kClass;
constexpr std::size_t kSelectionEntryField = offsetof(Selection, entries);
constexpr std::uint32_t kSelectionEntryClass = SelectionEntry::kClass;
constexpr std::size_t kSelectionEntryStride = sizeof(SelectionEntry);
constexpr std::size_t kSelectionChildOffset = offsetof(SelectionEntry, child);
// A line carries two takes of the same text.
constexpr std::uint32_t kLineClass = DialogueLine::kClass;
constexpr std::size_t kFirstTakeOffset = offsetof(DialogueLine, unk18);
constexpr std::size_t kSecondTakeOffset = kFirstTakeOffset + sizeof(TakeRecord);
constexpr std::size_t kLineByteCount = kFirstTakeOffset + sizeof(DialogueLine::unk18);
static_assert(sizeof(DialogueLine::unk18) == format::kDialogueTakeCount * sizeof(TakeRecord));
// Ceilings that stop a corrupt or cyclic tree from driving an unbounded walk.
constexpr std::size_t kMaximumDepth = 16;
constexpr std::size_t kMaximumObjects = 1024;
constexpr std::size_t kMaximumArrayRows = 4096;
// Authored totals are float sums of the take lengths, so they match within float rounding only.
constexpr float kSecondsTolerance = 0.001F;

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

/** Follows one signed self-relative pointer stored at `member`. @return False outside the blob. */
[[nodiscard]] bool
follow(std::span<const std::byte> bytes, std::size_t member, std::size_t& target) noexcept {
    std::int64_t relative = 0;
    if (!read_value(bytes, member, relative)) {
        return false;
    }
    if (relative >= 0) {
        const auto distance = static_cast<std::uint64_t>(relative);
        if (distance > bytes.size() - member) {
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

/** Reads one array field's data offset and count, checking its element class and stride. */
[[nodiscard]] bool read_array(std::span<const std::byte> bytes,
                              std::size_t field,
                              std::size_t stride,
                              std::uint32_t elementClass,
                              std::size_t& data,
                              std::size_t& count) noexcept {
    data = 0;
    count = 0;
    std::uint64_t rawCount = 0;
    std::size_t header = 0;
    std::uint64_t repeated = 0;
    std::uint32_t classId = 0;
    std::uint32_t padding = 0;
    if (!read_value(bytes, field, rawCount) || rawCount > kMaximumArrayRows) {
        return false;
    }
    if (rawCount == 0) {
        return true;
    }
    if (!follow(bytes, field + layout::kArrayFieldRelative, header)
        || !read_value(bytes, header, repeated) || repeated != rawCount
        || !read_value(bytes, header + offsetof(ArrayHeader, elementClass), classId)
        || classId != elementClass
        || !read_value(bytes, header + offsetof(ArrayHeader, padding), padding) || padding != 0) {
        return false;
    }
    data = header + sizeof(ArrayHeader);
    count = static_cast<std::size_t>(rawCount);
    return data <= bytes.size() && count * stride <= bytes.size() - data;
}

/** Reads one take stored at `offset`. */
[[nodiscard]] bool
read_take(std::span<const std::byte> bytes, std::size_t offset, Take& output) noexcept {
    return read_value(bytes, offset + offsetof(TakeRecord, unk0), output.audioTag)
           && read_value(bytes, offset + offsetof(TakeRecord, unk4), output.containerTag)
           && read_value(bytes, offset + offsetof(TakeRecord, unk8), output.stringHash)
           && read_value(bytes, offset + offsetof(TakeRecord, unkC), output.seconds)
           && std::isfinite(output.seconds) && output.seconds >= 0.0F;
}

/** Walk state shared by one tree: its hash, its object budget, and the lines it collects. */
struct Walk final {
    std::uint32_t definitionHash{};
    std::size_t objects{};
    std::vector<Line> lines{};
};

/**
 * Collects the lines below one tree object and computes how long it plays.
 * @return False when the object or any descendant is malformed.
 */
[[nodiscard]] bool walk_object(std::span<const std::byte> bytes,
                               std::size_t object,
                               std::size_t depth,
                               Walk& walk,
                               float& seconds) {
    seconds = 0.0F;
    std::uint32_t classId = 0;
    std::uint32_t hash = 0;
    if (depth > kMaximumDepth || ++walk.objects > kMaximumObjects || object < sizeof classId
        || !read_value(bytes, object - sizeof classId, classId)
        || !read_value(bytes, object + kObjectHashOffset, hash) || hash != walk.definitionHash) {
        return false;
    }
    if (classId == kLineClass) {
        Line line{};
        if (object + kLineByteCount > bytes.size()
            || !read_take(bytes, object + kFirstTakeOffset, line.first)
            || !read_take(bytes, object + kSecondTakeOffset, line.second)) {
            return false;
        }
        seconds = (std::max)(line.first.seconds, line.second.seconds);
        walk.lines.push_back(line);
        return true;
    }
    std::size_t data = 0;
    std::size_t count = 0;
    if (classId == kSequenceClass) {
        if (!read_array(bytes,
                        object + kSequenceChildField,
                        kPointerStride,
                        kPointerArrayClass,
                        data,
                        count)) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            std::size_t child = 0;
            float childSeconds = 0.0F;
            if (!follow(bytes, data + index * kPointerStride, child)
                || !walk_object(bytes, child, depth + 1U, walk, childSeconds)) {
                return false;
            }
            seconds += childSeconds;
        }
        return true;
    }
    if (classId == kSelectionClass) {
        if (!read_array(bytes,
                        object + kSelectionEntryField,
                        kSelectionEntryStride,
                        kSelectionEntryClass,
                        data,
                        count)) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            std::size_t child = 0;
            float childSeconds = 0.0F;
            if (!follow(bytes, data + index * kSelectionEntryStride + kSelectionChildOffset, child)
                || !walk_object(bytes, child, depth + 1U, walk, childSeconds)) {
                return false;
            }
            seconds = (std::max)(seconds, childSeconds);
        }
        return true;
    }
    return false;
}

/** One tree root and its hash, in blob order. */
struct Tree final {
    std::uint32_t definitionHash{};
    std::size_t root{};
};

} // namespace

/**
 * Matches each cue definition to the content trees that carry its hash.
 * @param list Authored dialogue list blob.
 * @param output Receives the cues, their lines and the shared hashes; reset first.
 * @return False when the definition or tree arrays are malformed.
 */
bool read(std::span<const std::byte> list, Snapshot& output) {
    output = {};
    std::vector<dialogue_group_index::Definition> definitions{};
    std::size_t treeRows = 0;
    std::size_t treeCount = 0;
    if (!dialogue_group_index::definitions(list, definitions)
        || !read_array(list,
                       kTreeField,
                       kTreeStride,
                       generated::DialogueTreeRow::kClass,
                       treeRows,
                       treeCount)) {
        return false;
    }
    std::vector<Tree> trees(treeCount);
    for (std::size_t index = 0; index < treeCount; ++index) {
        const std::size_t row = treeRows + index * kTreeStride;
        if (!read_value(list, row, trees[index].definitionHash)
            || !follow(list, row + offsetof(generated::DialogueTreeRow, root), trees[index].root)) {
            return false;
        }
    }
    const auto byHash = [](const Tree& row, const Tree& other) {
        return row.definitionHash < other.definitionHash;
    };
    std::stable_sort(trees.begin(), trees.end(), byHash);
    for (auto first = trees.begin(); first != trees.end();) {
        const auto last = std::find_if(first, trees.end(), [&](const Tree& tree) {
            return tree.definitionHash != first->definitionHash;
        });
        if (last - first > 1) {
            output.sharedHashes.push_back(
                {first->definitionHash, static_cast<std::uint32_t>(last - first)});
        }
        first = last;
    }

    output.cues.resize(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        Cue& cue = output.cues[index];
        cue.definitionHash = definitions[index].hash;
        cue.windowSeconds = definitions[index].authoredWindowSeconds;
        const auto range =
            std::equal_range(trees.begin(), trees.end(), Tree{cue.definitionHash, 0}, byHash);
        std::size_t matches = 0;
        bool malformed = false;
        std::vector<Line> matched{};
        std::vector<Line> unmatched{};
        for (auto tree = range.first; tree != range.second; ++tree) {
            Walk walk{};
            walk.definitionHash = cue.definitionHash;
            float seconds = 0.0F;
            if (!walk_object(list, tree->root, 0, walk, seconds)) {
                malformed = true;
                continue;
            }
            if (std::fabs(seconds - cue.windowSeconds) <= kSecondsTolerance) {
                ++matches;
                matched = std::move(walk.lines);
            } else {
                unmatched = std::move(walk.lines);
            }
        }
        const std::size_t candidates = static_cast<std::size_t>(range.second - range.first);
        if (candidates == 0) {
            cue.status = CueStatus::missing;
        } else if (matches == 1) {
            cue.status = CueStatus::resolved;
        } else if (malformed) {
            cue.status = CueStatus::malformed;
        } else if (candidates == 1) {
            // A sole tree is the cue's content even when its length disagrees; keep its lines.
            cue.status = CueStatus::mismatched;
            matched = std::move(unmatched);
        } else {
            cue.status = CueStatus::ambiguous;
        }
        if ((cue.status != CueStatus::resolved && cue.status != CueStatus::mismatched)
            || matched.size() > (std::numeric_limits<std::uint32_t>::max)()
            || output.lines.size() > (std::numeric_limits<std::uint32_t>::max)() - matched.size()) {
            continue;
        }
        cue.firstLine = static_cast<std::uint32_t>(output.lines.size());
        cue.lineCount = static_cast<std::uint32_t>(matched.size());
        output.lines.insert(output.lines.end(), matched.begin(), matched.end());
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::dialogue_list
