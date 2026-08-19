#include "common_state.h"

namespace sunrise::middleware::gameplay::external {
namespace {

/** Common epoch and activity values use full reflected 64-bit fields. */
constexpr std::uint8_t kValueWidth = 64;
/** The root count selects zero through three entries. */
constexpr std::uint8_t kCountWidth = 2;
/** Each activity entry ends with one reconciliation byte. */
constexpr std::uint8_t kGenerationWidth = 8;

} // namespace

/** Reads one common root without changing output on failure. */
bool read_common_state(encoding::bits::Reader& reader, CommonState& output) noexcept {
    CommonState candidate{};
    std::uint64_t value = 0;
    if (!reader.read(kValueWidth, candidate.patchEpoch[0])
        || !reader.read(kValueWidth, candidate.patchEpoch[1]) || !reader.read(kCountWidth, value)) {
        return false;
    }
    candidate.entryCount = static_cast<std::uint8_t>(value);
    for (std::size_t index = 0; index < candidate.entryCount; ++index) {
        if (!reader.read(kValueWidth, candidate.entries[index].activitySessionId)
            || !reader.read(kGenerationWidth, value)) {
            return false;
        }
        candidate.entries[index].reconciliationGeneration = static_cast<std::uint8_t>(value);
    }
    output = candidate;
    return true;
}

/** Writes one bounded common root. */
bool write_common_state(encoding::bits::Writer& writer, const CommonState& state) noexcept {
    if (state.entryCount > state.entries.size() || !writer.write(state.patchEpoch[0], kValueWidth)
        || !writer.write(state.patchEpoch[1], kValueWidth)
        || !writer.write(state.entryCount, kCountWidth)) {
        return false;
    }
    for (std::size_t index = 0; index < state.entryCount; ++index) {
        if (!writer.write(state.entries[index].activitySessionId, kValueWidth)
            || !writer.write(state.entries[index].reconciliationGeneration, kGenerationWidth)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::gameplay::external
