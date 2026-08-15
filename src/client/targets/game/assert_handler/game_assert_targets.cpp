#include "game_assert_targets.h"

#include <array>
#include <cstdint>

#include "../../../patterns/game/assert_handler/assert_signature_bytes.h"
#include "../assert_handler.h"

namespace sunrise::client::targets::game::assert_handler {
namespace {

Targets g_targets;
bool g_resolved{};

/** Byte-identical copies of the setter exist; this caps how many are checked. */
constexpr std::size_t kCandidateCapacity = 8;
/** Assert sites sampled for the vote; the image holds far more than this. */
constexpr std::size_t kSampleCapacity = 32;
/** Bytes searched above one assert site for a reference to a candidate slot. */
constexpr std::size_t kVoteWindow = 0x200;
/** A winner needs this many votes, which decoys never reach. */
constexpr std::size_t kMinimumVotes = 4;
/** Displacement of the setter's store operand. */
constexpr std::size_t kSlotDisplacementOffset = 14;
/** The setter's store instruction ends here, and displacements are relative to that. */
constexpr std::size_t kSlotInstructionEnd = 18;
/** A relative displacement is 4 bytes. */
constexpr std::size_t kDisplacementSize = 4;

/**
 * Reads one signed relative displacement.
 * @param address Displacement location.
 */
[[nodiscard]] std::int32_t displacement(const std::byte* address) noexcept {
    std::int32_t value = 0;
    for (std::size_t index = 0; index < kDisplacementSize; ++index) {
        value |= static_cast<std::int32_t>(static_cast<unsigned char>(address[index]))
                 << (8U * index);
    }
    return value;
}

/**
 * Resolves the slot a setter copy writes.
 * @param setter Matched setter body.
 * @return Slot address the setter stores into.
 */
[[nodiscard]] std::byte** slot_of(std::byte* setter) noexcept {
    std::byte* const target =
        setter + kSlotInstructionEnd + displacement(setter + kSlotDisplacementOffset);
    return reinterpret_cast<std::byte**>(target);
}

/**
 * Counts assert sites that reference one candidate slot.
 * @param candidate Slot written by one setter copy.
 * @param sites Sampled assert-site fatal tails.
 * @return Number of sites whose preceding window references the candidate.
 */
[[nodiscard]] std::size_t votes_for(const std::byte* const* candidate,
                                    std::span<std::byte* const> sites) noexcept {
    std::size_t votes = 0;
    for (std::byte* const site : sites) {
        // The site's guard load sits above its fatal tail; scan that window only.
        const std::size_t window = kVoteWindow;
        bool referenced = false;
        for (std::size_t offset = 0; offset + kDisplacementSize <= window && !referenced;
             ++offset) {
            const std::byte* const at = site - window + offset;
            const std::byte* const end = at + kDisplacementSize;
            referenced = end + displacement(at) == reinterpret_cast<const std::byte*>(candidate);
        }
        votes += referenced ? 1 : 0;
    }
    return votes;
}

} // namespace

/** Derives the assert handler slot by voting sampled assert sites against every setter copy. */
bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept {
    output = {};
    const patterns::Pattern setterPattern{"assert_set_handler",
                                          patterns::game::assert_handler::kSetHandler};
    const patterns::Pattern tailPattern{"assert_fatal_tail",
                                        patterns::game::assert_handler::kFatalTail};
    std::array<std::byte*, kCandidateCapacity> setters{};
    std::array<std::byte*, kSampleCapacity> sites{};
    const std::size_t setterCount = patterns::collect_matches(image, setterPattern, setters);
    const std::size_t siteCount = patterns::collect_matches(image, tailPattern, sites);
    if (setterCount == 0 || siteCount < kMinimumVotes) {
        return false;
    }

    const std::span<std::byte* const> sampled(sites.data(), siteCount);
    std::byte** winner = nullptr;
    std::size_t best = 0;
    std::size_t runnerUp = 0;
    for (std::size_t index = 0; index < setterCount; ++index) {
        std::byte** const candidate = slot_of(setters[index]);
        const std::size_t votes = votes_for(candidate, sampled);
        if (votes > best) {
            runnerUp = best;
            best = votes;
            winner = candidate;
        } else if (votes > runnerUp) {
            runnerUp = votes;
        }
    }
    if (winner == nullptr || best < kMinimumVotes || best == runnerUp) {
        return false;
    }

    Targets resolved;
    resolved.slot = winner;
    resolved.original = *winner;
    output = resolved;
    return true;
}

/** @param targets Validated assert table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved = true;
}

/** Clears the published assert target group. */
void clear() noexcept {
    g_targets = {};
    g_resolved = false;
}

/** @return Process-local assert target group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after the assert slot is published. */
bool is_resolved() noexcept {
    return g_resolved;
}

} // namespace sunrise::client::targets::game::assert_handler
