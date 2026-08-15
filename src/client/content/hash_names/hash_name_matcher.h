#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../state/build_data/hash_names/definition.h"

namespace sunrise::client::content::hash_names {

namespace names_state = state::build_data::hash_names;

/**
 * Named objects the class sweep finds. The measured live count is 241,586.
 * Neither a bubble nor a spawn set stores its own name, so names are recovered by hashing every
 * id the installed objects carry and matching it against the hashes they declare.
 */
inline constexpr std::size_t kTagCapacity = 262'144;
/**
 * Tags one slice reads, shared out between the readers.
 * The slice waits for every reader, so this bounds how long it blocks the frame. At the measured
 * cost of a read that is a few tens of milliseconds.
 */
inline constexpr std::size_t kBatchTags = 2048;
/** Longest id worth hashing. Anything longer cannot be a bubble name. */
inline constexpr std::size_t kRunCapacity = names_state::kNameLength;
/** Underscore-separated tokens one id may carry. */
inline constexpr std::size_t kTokenCapacity = 12;

/**
 * The target hashes and the rows one reader resolves against them.
 * Several readers match at once. The targets are shared and never written; the rows belong to one
 * reader alone and are merged when it joins, so nothing here locks.
 */
struct MatchState {
    /** Ascending unique target hashes, shared and read-only. */
    std::span<const std::uint32_t> targets;
    /** Rows parallel to the targets, written only by the reader that owns them. */
    std::span<names_state::Name> resolved;
};

/** Fixed working storage for one extraction pass, kept off the caller stack. */
struct Storage {
    /** Hashes the published destinations declare, ascending and unique. */
    std::array<std::uint32_t, names_state::kNameCapacity> targets{};
    std::size_t targetCount{};
    /** Resolved rows, parallel to targets. An unresolved row keeps a zero length. */
    std::array<names_state::Name, names_state::kNameCapacity> resolved{};
    std::size_t resolvedCount{};
    /** Named-object tags the sweep found, read in order so the block cache keeps hitting. */
    std::array<std::uint32_t, kTagCapacity> tags{};
    std::size_t tagCount{};
    /** Tag the next batch resumes from, so the pass continues across calls. */
    std::size_t cursor{};
    /** Tick the pass started on, so every report says what the pass cost. */
    std::uint64_t startedTick{};
    /** Set once the targets, the free name pass, and the sweep are done. */
    bool collected{};
    /** Set when the pass found more objects than storage holds, which no retry can fix. */
    bool invalid{};
};

/** Clears one extraction pass without freeing process-owned outer storage. */
void reset(Storage& storage) noexcept;

/**
 * Records every hash the published domains declare: bubble names and spawn-set names.
 * @param storage Pass storage receiving the ascending unique target set.
 * @return True when both domains are readable and their hashes fit.
 */
[[nodiscard]] bool collect_targets(Storage& storage) noexcept;

/**
 * Offers one id, keeping the shortest name that gives a target hash.
 * Every underscore-bounded prefix and suffix of the id is tried, because a bubble name is usually
 * one token of a longer object name.
 * @param state One reader's targets and rows.
 * @param text Candidate id.
 */
void offer(MatchState& state, std::string_view text) noexcept;

/**
 * Offers every id inside one blob.
 * @param state One reader's targets and rows.
 * @param blob Whole object bytes.
 */
void offer_blob(MatchState& state, std::span<const std::byte> blob) noexcept;

/** @param storage Pass storage. @return Its targets and its own rows. */
[[nodiscard]] MatchState match_state(Storage& storage) noexcept;

/**
 * Folds one reader's rows into the pass, keeping the shortest name for every target.
 * @param storage Pass storage.
 * @param rows Rows one reader resolved, parallel to the targets.
 */
void merge(Storage& storage, std::span<const names_state::Name> rows) noexcept;

/**
 * Adds one swept tag to an unfinished pass.
 * @param storage Pass storage.
 * @param tag Named-object tag.
 * @return False when the pass is finished or storage is full.
 */
[[nodiscard]] bool add_tag(Storage& storage, std::uint32_t tag) noexcept;

/**
 * Packs the resolved rows into ascending hash order for publishing.
 * @param storage Pass storage whose resolved count is set here.
 */
void finish(Storage& storage) noexcept;

} // namespace sunrise::client::content::hash_names
