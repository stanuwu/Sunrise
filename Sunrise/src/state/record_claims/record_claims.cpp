#include <atomic>

#include "../build_data/nodes/node_persistence.h"
#include "record_claims.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../build_data/nodes/definition.h"
#include "../build_data/nodes/node_catalog.h"
#include "../build_data/runtime.h"
#include "../unlocks/definition.h"

namespace sunrise::state::record_claims {
namespace {

/** One bit per addressable account flag index, which is cheaper than a set and never allocates. */
constexpr std::size_t kIndexCapacity = unlocks::kAccountFlagCapacity;
constexpr std::size_t kWordBits = 64;
constexpr std::size_t kWordCount = (kIndexCapacity + kWordBits - 1) / kWordBits;

/** The claim file lives beside the build data cache, which already owns this directory. */
constexpr std::wstring_view kClaimFileSuffix = L"\\cache\\record_claims.bin";
/** Identifies the file on sight, so an unrelated file of the right length cannot be read as one. */
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'C', 'L', 'M', '1'};
/** A claim is a flag bank row and the score its record carries. */
constexpr std::size_t kEntrySize = 2 * sizeof(std::uint16_t);
/** Far above the 2242 records the build ships, and small enough to read in one go. */
constexpr std::uint32_t kMaximumEntries = 8192;

std::mutex g_lock;
std::array<std::uint64_t, kWordCount> g_claimed{};
std::array<std::uint16_t, kIndexCapacity> g_scoreByIndex{};
std::size_t g_count{};
std::uint32_t g_score{};
core::path::Buffer g_path{};
bool g_pathReady{};

void report(const char* stage, const char* result, std::size_t detail) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=claims stage=%s result=%s entries=%zu", stage, result, detail);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         std::string_view{result} == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Writes every held claim. The caller holds the lock. */
void store_locked() noexcept {
    if (!g_pathReady) {
        return;
    }
    std::vector<char> document{};
    document.insert(document.end(), kMagic.begin(), kMagic.end());
    const auto entries = static_cast<std::uint32_t>(g_count);
    const auto* entryBytes = reinterpret_cast<const char*>(&entries);
    document.insert(document.end(), entryBytes, entryBytes + sizeof entries);
    for (std::size_t word = 0; word < g_claimed.size(); ++word) {
        std::uint64_t bits = g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const std::size_t index = word * kWordBits + offset;
            const auto packedIndex = static_cast<std::uint16_t>(index);
            const std::uint16_t packedScore = g_scoreByIndex[index];
            const auto* indexBytes = reinterpret_cast<const char*>(&packedIndex);
            const auto* scoreBytes = reinterpret_cast<const char*>(&packedScore);
            document.insert(document.end(), indexBytes, indexBytes + sizeof packedIndex);
            document.insert(document.end(), scoreBytes, scoreBytes + sizeof packedScore);
        }
    }

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("store", "open_fail", g_count);
        return;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    report("store", complete ? "ok" : "write_fail", g_count);
}

/** Reads every claim the file holds. The caller holds the lock. */
void load_locked() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // No file is an account that has claimed nothing, which is the first-run state.
        report("load", "absent", 0);
        return;
    }
    std::array<char, sizeof(kMagic) + sizeof(std::uint32_t)> header{};
    DWORD read = 0;
    if (ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) == FALSE
        || read != header.size()
        || std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
        (void)CloseHandle(file);
        report("load", "header_fail", 0);
        return;
    }
    std::uint32_t entries = 0;
    std::memcpy(&entries, header.data() + kMagic.size(), sizeof entries);
    if (entries > kMaximumEntries) {
        (void)CloseHandle(file);
        report("load", "count_fail", entries);
        return;
    }
    std::vector<char> payload(static_cast<std::size_t>(entries) * kEntrySize);
    read = 0;
    const bool readOk =
        payload.empty()
        || (ReadFile(file, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr)
                != FALSE
            && read == payload.size());
    (void)CloseHandle(file);
    if (!readOk) {
        report("load", "read_fail", entries);
        return;
    }

    std::size_t restored = 0;
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        std::uint16_t index = 0;
        std::uint16_t score = 0;
        std::memcpy(&index, payload.data() + static_cast<std::size_t>(entry) * kEntrySize,
                    sizeof index);
        std::memcpy(&score,
                    payload.data() + static_cast<std::size_t>(entry) * kEntrySize + sizeof index,
                    sizeof score);
        if (static_cast<std::size_t>(index) >= kIndexCapacity) {
            continue;
        }
        const std::size_t word = static_cast<std::size_t>(index) / kWordBits;
        const std::uint64_t bit = std::uint64_t{1}
                                  << (static_cast<std::size_t>(index) % kWordBits);
        if ((g_claimed[word] & bit) != 0) {
            continue;
        }
        g_claimed[word] |= bit;
        g_scoreByIndex[index] = score;
        g_score += score;
        ++g_count;
        ++restored;
    }
    report("load", "ok", restored);
}

} // namespace

/** Derives the claim file path and loads any claims already held. */
bool initialize(void* module) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_pathReady = false;
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kClaimFileSuffix)) {
        report("initialize", "path_fail", 0);
        return false;
    }
    g_pathReady = true;
    load_locked();
    return true;
}

/** Forgets every held claim, in memory only. */
void clear() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_claimed.fill(0);
    g_scoreByIndex.fill(0);
    g_count = 0;
    g_score = 0;
}

/** Marks one account flag bank index claimed, adds its score, and writes the claim file. */
bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    const std::lock_guard<std::mutex> guard(g_lock);
    if ((g_claimed[word] & bit) == 0) {
        g_claimed[word] |= bit;
        g_scoreByIndex[flagIndex] = scoreValue;
        ++g_count;
        // Only a first claim scores, so a repeated click cannot inflate the total.
        g_score += scoreValue;
        // Written per claim rather than at shutdown: a crash must not lose what the client is
        // already showing as Acquired.
        store_locked();
    }
    return true;
}

/** Lays every held claim over one account flag bank. */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept {
    std::size_t changed = 0;
    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t word = 0; word < g_claimed.size(); ++word) {
        std::uint64_t bits = g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const std::size_t index = word * kWordBits + offset;
            // A bank shorter than the index space is not an error: the tail simply is not sent.
            if (index >= accountFlags.size()) {
                continue;
            }
            if (accountFlags[index] != unlocks::kFlagSet) {
                accountFlags[index] = unlocks::kFlagSet;
                ++changed;
            }
        }
    }
    return changed;
}

namespace {

/** Carries the bank and a tally through the node walk, which takes a plain function pointer. */
struct NodeProgress {
    std::span<std::int32_t> values;
    std::size_t written;
    /** One entry per value index, non-zero where a category's own bar reads. */
    std::span<const char> categories;
};

/** True when this account flag bank row is held. The caller owns the claim lock. */
[[nodiscard]] bool claimed_locked(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    return (g_claimed[word] & bit) != 0;
}

} // namespace

/**
 * @return How many of a category's children are claimed. The caller owns the claim lock.
 *
 * Both passes count the same thing against different banks, so they share the count rather than
 * each carrying a copy of the loop.
 */
[[nodiscard]] std::int32_t claimed_children(const build_data::nodes::Definition& node) noexcept {
    std::int32_t claimed = 0;
    for (std::size_t child = 0; child < node.childCount; ++child) {
        build_data::records::Definition record{};
        if (build_data::find_record_definition(node.children[child], record)
            && record.completionFlagIndex != build_data::records::kUnavailableFlagIndex
            && claimed_locked(record.completionFlagIndex)) {
            ++claimed;
        }
    }
    return claimed;
}

/**
 * @return One when a category's parent record is itself claimed, otherwise zero.
 *
 * The parent is the child naming the category's own value slot. It sits in the category's count and
 * not in its own bar's, so subtracting it is what separates the two totals. The caller owns the
 * claim lock.
 */
[[nodiscard]] std::int32_t claimed_parent(const build_data::nodes::Definition& node) noexcept {
    for (std::size_t child = 0; child < node.childCount; ++child) {
        build_data::records::Definition record{};
        if (build_data::find_record_definition(node.children[child], record)
            && record.completionFlagIndex != build_data::records::kUnavailableFlagIndex
            && record.categoryValueIndex == node.valueIndex
            && claimed_locked(record.completionFlagIndex)) {
            return 1;
        }
    }
    return 0;
}

/** Writes each node's claimed-child count into the value slot its bar reads. */
std::size_t apply_node_progress(std::span<std::int32_t> objectiveValues) noexcept {
    // The account image is the latest point anything asks for the node table, and on a warm start it
    // may still be unpublished: the content pass is skipped and an earlier publish can be refused.
    // Latches on success, so a run that has its table pays nothing.
    static std::atomic<bool> nodesPublished{false};
    if (!nodesPublished.load(std::memory_order_relaxed)
        && build_data::nodes::load_and_publish()) {
        nodesPublished.store(true, std::memory_order_relaxed);
    }
    // Every category's own index, so the walk can tell a free slot above a category from the next
    // category along. Without it, writing the slot above drives whichever book owns that slot.
    std::vector<char> categoryFlags(objectiveValues.size(), 0);
    build_data::nodes::for_each(
        &categoryFlags, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* flags = static_cast<std::vector<char>*>(context);
            const std::uint16_t index = node.valueIndex;
            if (index != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::size_t>(index) < flags->size()) {
                (*flags)[index] = 1;
            }
        });

    NodeProgress progress{objectiveValues, 0, std::span<const char>{categoryFlags}};
    // The claim lock is taken first and the node lock inside the walk. Nothing takes them the other
    // way round, so the order cannot close a cycle.
    const std::lock_guard<std::mutex> guard(g_lock);
    build_data::nodes::for_each_driving(
        &progress, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* state = static_cast<NodeProgress*>(context);
            // Only the lore books are counted. Every other category that drives a bar keeps what
            // the authored policy gave it: node 896 carries an authored -1 at its value index, and
            // writing a count over it replaced a deliberate sentinel with a zero.
            if (!build_data::nodes::lore_category(node.definitionIndex)) {
                return;
            }
            // Two counts, because a category and its parent record keep separate bars. The category
            // counts every child it owns, the parent among them; the parent's own bar counts only
            // the chapters, which is why its denominator is one lower. The parent is the child
            // naming the category's own value slot.
            const std::int32_t claimed = claimed_children(node);
            const std::int32_t claimedChapters = claimed - claimed_parent(node);
            // The parent's bar sits one slot above its category on the books where that slot is
            // free, confirmed by marker on two of them. Categories are allocated contiguously
            // though, so for many books the slot above is the next book's category, and writing it
            // drove the wrong book's bar. Those are skipped: a bar left at zero is wrong, a bar
            // carrying another book's count is worse. Where those parents read is not yet known.
            if (static_cast<std::size_t>(node.parentValueIndex) < state->values.size()
                && state->categories[node.parentValueIndex] == 0) {
                state->values[node.parentValueIndex] = claimedChapters;
                ++state->written;
            }
            if (static_cast<std::size_t>(node.valueIndex) < state->values.size()) {
                state->values[node.valueIndex] = claimed;
                ++state->written;
            }
        });
    return progress.written;
}

/** Writes each category's claimed-child count into the character value slot its bar reads. */
std::size_t apply_character_node_progress(std::span<std::int32_t> characterValues) noexcept {
    NodeProgress progress{characterValues, 0, {}};
    // Same lock order as the account pass: claims first, catalog inside the walk.
    const std::lock_guard<std::mutex> guard(g_lock);
    build_data::nodes::for_each(
        &progress, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* state = static_cast<NodeProgress*>(context);
            if (!build_data::nodes::lore_category(node.definitionIndex)
                || node.characterValueIndex == build_data::nodes::kUnavailableValueIndex
                || static_cast<std::size_t>(node.characterValueIndex) >= state->values.size()) {
                return;
            }
            state->values[node.characterValueIndex] = claimed_children(node);
            ++state->written;
        });
    return progress.written;
}

/** @return Total score of every held claim. */
std::uint32_t total_score() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_score;
}

/** @return Number of distinct indices held. */
std::size_t count() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_count;
}

} // namespace sunrise::state::record_claims
