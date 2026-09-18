#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::client::content::activity::sdk_generation::dialogue_list {

/** One recorded take of a line: its audio, its localized text, and its length. */
struct Take final {
    std::uint32_t audioTag{};
    std::uint32_t containerTag{};
    std::uint32_t stringHash{};
    float seconds{};
};

/** One line. Both takes carry the same text; the line lasts as long as the longer take. */
struct Line final {
    Take first{};
    Take second{};
};

/** How one cue definition was matched to the content tree the list authors for it. */
enum class CueStatus : std::uint8_t {
    /** Exactly one tree carrying the definition hash matches the authored duration. */
    resolved,
    /** Several trees share the definition hash and none or several match the authored duration. */
    ambiguous,
    /** No tree carries the definition hash. */
    missing,
    /** The matched tree is malformed or its duration differs from the authored one. */
    malformed,
};

/** One cue in list order. Lines are present only for resolved cues. */
struct Cue final {
    std::uint32_t definitionHash{};
    /** Authored total: a sequence adds its children, a selection lasts its longest branch. */
    float seconds{};
    std::uint32_t firstLine{};
    std::uint32_t lineCount{};
    CueStatus status{CueStatus::missing};
};

/** One definition hash carried by more than one content tree. */
struct SharedHash final {
    std::uint32_t definitionHash{};
    std::uint32_t treeCount{};
};

/** Every cue of one dialogue list, its lines in cue order, and the hashes several trees share. */
struct Snapshot final {
    std::vector<Cue> cues{};
    std::vector<Line> lines{};
    std::vector<SharedHash> sharedHashes{};
};

/**
 * Reads one dialogue list blob.
 * @return False when the definition or tree arrays are malformed; a bad tree only marks its cues.
 */
[[nodiscard]] bool read(std::span<const std::byte> list, Snapshot& output);

} // namespace sunrise::client::content::activity::sdk_generation::dialogue_list
