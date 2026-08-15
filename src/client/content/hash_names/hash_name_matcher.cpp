#include "hash_name_matcher.h"

#include <algorithm>

#include "../../../state/build_data/runtime.h"

namespace sunrise::client::content::hash_names {
namespace {

namespace layouts = state::build_data::scenarios;
namespace spawn_state = state::build_data::spawn_sets;

/** FNV-1 32-bit offset basis. The engine hashes names with FNV-1, not FNV-1a. */
constexpr std::uint32_t kHashBasis = 0x811C9DC5U;
/** FNV-1 32-bit prime. */
constexpr std::uint32_t kHashPrime = 0x01000193U;

/** @return The FNV-1 hash of text, which is what a bubble carries. */
[[nodiscard]] std::uint32_t hash_of(std::string_view text) noexcept {
    std::uint32_t value = kHashBasis;
    for (const char character : text) {
        value = (value * kHashPrime) ^ static_cast<std::uint8_t>(character);
    }
    return value;
}

/**
 * Records one candidate against its target row.
 * @param state One reader's targets and rows.
 * @param text Candidate id that hashes to a target.
 * @param slot Target row index.
 */
void keep(MatchState& state, std::string_view text, std::size_t slot) noexcept {
    names_state::Name& row = state.resolved[slot];
    // The shortest candidate is the bare name; a longer one is a suffix of some object's name.
    if (row.nameLength != 0 && row.nameLength <= text.size()) {
        return;
    }
    row = {};
    row.hash = state.targets[slot];
    std::copy_n(text.begin(), text.size(), row.name.begin());
    row.nameLength = static_cast<std::uint8_t>(text.size());
}

/**
 * Finds one hash in the ascending target set.
 * @param state One reader's targets and rows.
 * @param slot Receives the target row index.
 * @return True when the hash is a target.
 */
[[nodiscard]] bool
target_slot(const MatchState& state, std::uint32_t hash, std::size_t& slot) noexcept {
    const auto first = state.targets.begin();
    const auto last = state.targets.end();
    const auto found = std::lower_bound(first, last, hash);
    if (found == last || *found != hash) {
        return false;
    }
    slot = static_cast<std::size_t>(found - first);
    return true;
}

} // namespace

/** Clears one extraction pass without freeing process-owned outer storage. */
void reset(Storage& storage) noexcept {
    storage.targets.fill(0);
    storage.targetCount = 0;
    storage.resolved.fill(names_state::Name{});
    storage.resolvedCount = 0;
    storage.tagCount = 0;
    storage.cursor = 0;
    storage.collected = false;
    storage.invalid = false;
}

/** Records every hash the published domains declare: bubble names and spawn-set names. */
bool collect_targets(Storage& storage) noexcept {
    storage.targets.fill(0);
    storage.targetCount = 0;
    // One whole domain of fixed rows, so the snapshot is static rather than a local.
    static std::array<layouts::Definition, layouts::kDefinitionCapacity> scratch{};
    std::size_t rows = 0;
    if (!state::build_data::snapshot_scenario_layouts(scratch, rows)) {
        return false;
    }
    for (std::size_t row = 0; row < rows; ++row) {
        const layouts::Definition& layout = scratch[row];
        const std::size_t declared =
            (std::min)(static_cast<std::size_t>(layout.bubbleCount), layout.bubbleHashes.size());
        for (std::size_t bubble = 0; bubble < declared; ++bubble) {
            const std::uint32_t hash = layout.bubbleHashes[bubble];
            if (hash == 0 || storage.targetCount >= storage.targets.size()) {
                continue;
            }
            storage.targets[storage.targetCount++] = hash;
        }
    }
    // The spawn sets name themselves the same way, and this pass already reads every blob that
    // could carry one, so their hashes ride along at no extra read.
    static std::array<spawn_state::NameHash, spawn_state::kNameHashCapacity> spawns{};
    std::size_t spawnCount = 0;
    if (state::build_data::snapshot_spawn_name_hashes(spawns, spawnCount)) {
        for (std::size_t row = 0; row < spawnCount; ++row) {
            if (spawns[row].value != 0 && storage.targetCount < storage.targets.size()) {
                storage.targets[storage.targetCount++] = spawns[row].value;
            }
        }
    }
    auto end = storage.targets.begin() + static_cast<std::ptrdiff_t>(storage.targetCount);
    std::sort(storage.targets.begin(), end);
    storage.targetCount = static_cast<std::size_t>(std::unique(storage.targets.begin(), end)
                                                   - storage.targets.begin());
    return storage.targetCount != 0;
}

/** Offers one id to the pass, keeping the shortest name that gives a target hash. */
void offer(MatchState& state, std::string_view text) noexcept {
    if (text.empty() || text.size() > kRunCapacity) {
        return;
    }
    // Token boundaries, so every underscore-bounded prefix and suffix can be tried.
    std::array<std::size_t, kTokenCapacity> starts{};
    std::size_t tokens = 0;
    starts[tokens++] = 0;
    for (std::size_t index = 0; index < text.size() && tokens < starts.size(); ++index) {
        if (text[index] == '_') {
            starts[tokens++] = index + 1;
        }
    }
    for (std::size_t first = 0; first < tokens; ++first) {
        for (std::size_t last = tokens; last > first; --last) {
            const std::size_t begin = starts[first];
            // A suffix run ends at the token separator before the next start, or at the end.
            const std::size_t end = last == tokens ? text.size() : starts[last] - 1;
            if (end <= begin) {
                continue;
            }
            const std::string_view candidate = text.substr(begin, end - begin);
            std::size_t slot = 0;
            if (target_slot(state, hash_of(candidate), slot)) {
                keep(state, candidate, slot);
            }
        }
    }
}

/** Offers every id inside one blob. */
void offer_blob(MatchState& state, std::span<const std::byte> blob) noexcept {
    std::size_t start = 0;
    std::size_t length = 0;
    for (std::size_t index = 0; index <= blob.size(); ++index) {
        const char character = index < blob.size() ? static_cast<char>(blob[index]) : '\0';
        if (index < blob.size() && names_state::name_character(character)) {
            if (length == 0) {
                start = index;
            }
            ++length;
            continue;
        }
        if (length != 0 && length <= kRunCapacity) {
            offer(state,
                  std::string_view(reinterpret_cast<const char*>(blob.data()) + start, length));
        }
        length = 0;
    }
}

/** @param storage Pass storage. @return Its targets and its own rows. */
MatchState match_state(Storage& storage) noexcept {
    return MatchState{{storage.targets.data(), storage.targetCount},
                      {storage.resolved.data(), storage.targetCount}};
}

/** Folds one reader's rows into the pass, keeping the shortest name for every target. */
void merge(Storage& storage, std::span<const names_state::Name> rows) noexcept {
    const std::size_t count = (std::min)(rows.size(), storage.targetCount);
    for (std::size_t slot = 0; slot < count; ++slot) {
        const names_state::Name& candidate = rows[slot];
        names_state::Name& row = storage.resolved[slot];
        if (candidate.nameLength == 0
            || (row.nameLength != 0 && row.nameLength <= candidate.nameLength)) {
            continue;
        }
        row = candidate;
    }
}

/** Adds one swept tag to an unfinished pass. */
bool add_tag(Storage& storage, std::uint32_t tag) noexcept {
    if (storage.collected || storage.invalid || tag == 0) {
        return false;
    }
    if (storage.tagCount >= storage.tags.size()) {
        storage.invalid = true;
        return false;
    }
    storage.tags[storage.tagCount++] = tag;
    return true;
}

/** Packs the resolved rows into ascending hash order for publishing. */
void finish(Storage& storage) noexcept {
    std::size_t kept = 0;
    for (std::size_t row = 0; row < storage.targetCount; ++row) {
        if (storage.resolved[row].nameLength != 0) {
            storage.resolved[kept++] = storage.resolved[row];
        }
    }
    // The targets are already ascending and unique, so the kept rows keep that order.
    for (std::size_t row = kept; row < storage.resolved.size(); ++row) {
        storage.resolved[row] = {};
    }
    storage.resolvedCount = kept;
}

} // namespace sunrise::client::content::hash_names
