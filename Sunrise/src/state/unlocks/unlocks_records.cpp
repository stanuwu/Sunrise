#include "unlocks_records.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>

#include "../build_data/nodes/definition.h"
#include "../build_data/nodes/node_catalog.h"
#include "../build_data/records/record_catalog.h"
#include "../build_data/runtime.h"
#include "../investment/store.h"
#include "definition.h"
#include "unlocks_runtime.h"

namespace sunrise::state::unlocks::records {
namespace {

namespace catalog = build_data::records;
namespace node_catalog = build_data::nodes;

/** A flag index no record's completion flag names carries this instead of a record row. */
constexpr std::uint16_t kUnavailableRecordRow = 0xFFFFU;

/** Objective values one record can occupy. The widest installed record names six. */
constexpr std::size_t kObjectiveRunCapacity = 16;

/**
 * Books where the shared counter itself grants chapters, so no per-chapter slot holds one.
 * TODO: derive; no package predicate separates these from books whose bar counts claims.
 */
constexpr std::array<std::uint16_t, 4> kCounterGrantedLoreNodes{
    823U, // Stolen Intelligence
    839U, // Unveiling
    850U, // A Man with No Name
    853U, // Revelation
};

/** Record row of each account completion flag, rebuilt when a catalog is replaced. */
std::array<std::uint16_t, kAccountFlagCapacity> g_recordByFlag{};

/** True for each completion flag owned by a counter-granted lore book. */
std::array<bool, kAccountFlagCapacity> g_counterGranted{};

/** One record's definition hash and its row. */
struct HashedRecord {
    std::uint32_t definitionHash{};
    std::uint16_t row{};
};

/** Every record row in ascending hash, then row, order, rebuilt when a catalog is replaced. */
std::array<HashedRecord, catalog::kDefinitionCapacity> g_recordByHash{};

/** Rows of g_recordByHash in use. */
std::size_t g_recordByHashCount{};

/** The caches are read and written only under the unlock banks' exclusive lock. */
bool g_cacheReady{};

/** @return True when that lore node's book counts bars instead of claims. */
[[nodiscard]] constexpr bool counter_granted_node(std::uint16_t nodeIndex) noexcept {
    return std::find(kCounterGrantedLoreNodes.begin(), kCounterGrantedLoreNodes.end(), nodeIndex)
           != kCounterGrantedLoreNodes.end();
}

/** Fills the record maps and the counter-granted mask from the published catalogs. */
void build_cache() noexcept {
    g_recordByFlag.fill(kUnavailableRecordRow);
    g_counterGranted.fill(false);
    g_recordByHashCount = 0;
    if (!build_data::record_definitions_ready()) {
        return;
    }
    const std::size_t rows = catalog::count();
    for (std::size_t row = 0; row < rows; ++row) {
        catalog::Definition record{};
        if (!catalog::find(static_cast<std::uint16_t>(row), record)) {
            continue;
        }
        g_recordByHash[g_recordByHashCount++] = {record.definitionHash,
                                                 static_cast<std::uint16_t>(row)};
        if (record.completionFlagIndex < g_recordByFlag.size()
            && g_recordByFlag[record.completionFlagIndex] == kUnavailableRecordRow) {
            g_recordByFlag[record.completionFlagIndex] = static_cast<std::uint16_t>(row);
        }
    }
    std::sort(g_recordByHash.begin(),
              g_recordByHash.begin() + static_cast<std::ptrdiff_t>(g_recordByHashCount),
              [](const HashedRecord& left, const HashedRecord& right) noexcept {
                  return left.definitionHash != right.definitionHash
                             ? left.definitionHash < right.definitionHash
                             : left.row < right.row;
              });
    if (!build_data::node_definitions_ready()) {
        return;
    }
    node_catalog::for_each(
        &g_counterGranted, [](void* context, const node_catalog::Definition& node) noexcept {
            if (!counter_granted_node(node.definitionIndex)) {
                return;
            }
            auto* granted = static_cast<std::array<bool, kAccountFlagCapacity>*>(context);
            for (std::size_t child = 0; child < node.childCount; ++child) {
                catalog::Definition record{};
                if (build_data::find_record_definition(node.children[child], record)
                    && record.loreRow != catalog::kUnavailableLoreRow
                    && record.completionFlagIndex < granted->size()) {
                    (*granted)[record.completionFlagIndex] = true;
                }
            }
        });
    g_cacheReady = true;
}

/** Never call this from inside a catalog walk: it re-enters the node catalog's shared lock. */
void ensure_cache() noexcept {
    if (!g_cacheReady) {
        build_cache();
    }
}

[[nodiscard]] bool find_by_flag(std::uint16_t flagIndex, catalog::Definition& record) noexcept {
    if (flagIndex >= g_recordByFlag.size()) {
        return false;
    }
    const std::uint16_t row = g_recordByFlag[flagIndex];
    return row != kUnavailableRecordRow && catalog::find(row, record);
}

/** Finds the lowest record row with one definition hash. */
[[nodiscard]] bool
find_by_hash(std::uint32_t definitionHash, catalog::Definition& record) noexcept {
    const auto rows = std::span(g_recordByHash).first(g_recordByHashCount);
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), definitionHash, [](const HashedRecord& row, std::uint32_t hash) {
            return row.definitionHash < hash;
        });
    // A row cached before a catalog replace may hold another record now.
    return found != rows.end() && found->definitionHash == definitionHash
           && catalog::find(found->row, record) && record.definitionHash == definitionHash;
}

[[nodiscard]] bool counter_granted_flag(std::uint16_t flagIndex) noexcept {
    return flagIndex < g_counterGranted.size() && g_counterGranted[flagIndex];
}

/**
 * Reads the objective values one record occupies, whether or not it names objective rows.
 * A record with no objective rows still reserves its run, clamped to its last interval step.
 * @param record Record whose run is read.
 * @param values Receives one value index and threshold per reserved slot.
 * @return How many slots the record occupies.
 */
[[nodiscard]] std::size_t record_objectives(const catalog::Definition& record,
                                            std::span<catalog::Objective> values) noexcept {
    std::size_t written = 0;
    if (record.objectiveCount != 0) {
        for (std::uint8_t row = 0; row < record.objectiveCount && written < values.size(); ++row) {
            if (catalog::objective(record, row, values[written])) {
                ++written;
            }
        }
        return written;
    }
    catalog::Interval last{};
    if (record.intervalCount == 0
        || !catalog::interval(record, static_cast<std::size_t>(record.intervalCount) - 1U, last)) {
        return 0;
    }
    for (std::uint8_t slot = 0;
         slot < catalog::kReservedObjectiveValueCount && written < values.size();
         ++slot) {
        values[written] = {last.completionValue,
                           static_cast<std::uint16_t>(record.objectiveValueIndex + slot),
                           catalog::kUnavailableValueIndex,
                           -1};
        ++written;
    }
    return written;
}

[[nodiscard]] bool flag_set(const Table& table, std::uint16_t flagIndex) noexcept {
    return flagIndex < table.accountFlags.size() && table.accountFlags[flagIndex] == kFlagSet;
}

/** One interval record's steps and the cumulative progress value every step reads. */
struct IntervalLadder {
    std::uint16_t progressValueIndex{};
    std::array<catalog::Interval, catalog::kIntervalPerRecordCapacity> steps{};
};

/**
 * Resolves an interval record's progress value and steps. Every objective row must read one
 * shared value; a record with no rows reads the first value of its own run.
 * @return False when the rows read different values, the progress value is the redeemed count,
 *         or the steps do not rise.
 */
[[nodiscard]] bool resolve_ladder(const Table& table,
                                  const catalog::Definition& record,
                                  IntervalLadder& ladder) noexcept {
    ladder = {};
    if (record.intervalCount == 0 || record.intervalCount > ladder.steps.size()
        || record.redeemedCountValueIndex >= table.objectiveValues.size()) {
        return false;
    }
    ladder.progressValueIndex = record.objectiveValueIndex;
    for (std::uint8_t row = 0; row < record.objectiveCount; ++row) {
        catalog::Objective objective{};
        if (!catalog::objective(record, row, objective)) {
            return false;
        }
        const std::uint16_t source = objective.sourceValueIndex != catalog::kUnavailableValueIndex
                                         ? objective.sourceValueIndex
                                         : objective.valueIndex;
        if (row != 0 && source != ladder.progressValueIndex) {
            return false;
        }
        ladder.progressValueIndex = source;
    }
    if (ladder.progressValueIndex >= table.objectiveValues.size()
        || ladder.progressValueIndex == record.redeemedCountValueIndex) {
        return false;
    }
    std::int32_t previous = 0;
    for (std::uint8_t step = 0; step < record.intervalCount; ++step) {
        if (!catalog::interval(record, step, ladder.steps[step])
            || ladder.steps[step].completionValue <= previous) {
            return false;
        }
        previous = ladder.steps[step].completionValue;
    }
    return true;
}

/** @return True when every objective slot of the record reads at or above its threshold. */
[[nodiscard]] bool complete(const Table& table, const catalog::Definition& record) noexcept {
    if (counter_granted_flag(record.completionFlagIndex)) {
        // These chapters share one counter slot, so no per-chapter value can hold completion.
        return false;
    }
    std::array<catalog::Objective, kObjectiveRunCapacity> objectives{};
    const std::size_t slots = record_objectives(record, objectives);
    if (slots == 0) {
        return false;
    }
    for (std::size_t at = 0; at < slots; ++at) {
        const std::size_t slot = objectives[at].valueIndex;
        if (objectives[at].completionValue <= 0 || slot >= table.objectiveValues.size()
            || table.objectiveValues[slot] < objectives[at].completionValue) {
            return false;
        }
    }
    return true;
}

/** Writes one record's whole objective run. A counter-granted chapter shares its slot. */
void write_objective_run(Table& table,
                         const catalog::Definition& record,
                         std::int32_t value,
                         bool clampToThreshold) noexcept {
    if (counter_granted_flag(record.completionFlagIndex)) {
        return;
    }
    std::array<catalog::Objective, kObjectiveRunCapacity> objectives{};
    const std::size_t slots = record_objectives(record, objectives);
    for (std::size_t at = 0; at < slots; ++at) {
        const std::size_t slot = objectives[at].valueIndex;
        if (slot >= table.objectiveValues.size()) {
            continue;
        }
        table.objectiveValues[slot] =
            clampToThreshold ? (std::min)(value, objectives[at].completionValue) : value;
    }
}

/** Counts of one lore node's chapters, read from the live banks. */
struct ChapterCounts {
    std::int32_t claimed{};
    std::int32_t collected{};
    std::int32_t allCollected{};
    std::int32_t claimedParent{};
    bool cumulative{};
};

/**
 * Counts one lore node's claimed and collected chapters from the live banks.
 * @param node Presentation node whose children are the chapters.
 * @return Zeroed counts when no child names a completion flag.
 */
[[nodiscard]] ChapterCounts chapter_counts(const Table& table,
                                           const node_catalog::Definition& node) noexcept {
    ChapterCounts counts{};
    for (std::size_t child = 0; child < node.childCount; ++child) {
        catalog::Definition record{};
        if (!build_data::find_record_definition(node.children[child], record)
            || record.completionFlagIndex == catalog::kUnavailableFlagIndex) {
            continue;
        }
        const bool isClaimed = flag_set(table, record.completionFlagIndex);
        const bool isCollected = isClaimed || complete(table, record);
        counts.allCollected += isCollected;
        if (record.loreRow == catalog::kUnavailableLoreRow) {
            counts.claimedParent += isClaimed && record.categoryValueIndex == node.valueIndex;
            continue;
        }
        counts.claimed += isClaimed;
        counts.collected += isCollected;
        catalog::Objective objective{};
        counts.cumulative =
            counts.cumulative
            || (catalog::objective(record, 0, objective) && objective.completionValue > 1);
    }
    return counts;
}

/** One authored reward milestone at the head of a presentation node's child list. */
struct RewardMilestone {
    std::uint16_t completionFlagIndex{};
    std::uint16_t objectiveSlot{};
    std::int32_t completionValue{};
};

/**
 * Resolves the shape shared by node-level reward milestones: a zero-score reward record with one
 * positive objective. The node walk additionally requires a descending consecutive cluster.
 */
[[nodiscard]] bool resolve_reward_milestone(std::uint16_t recordIndex,
                                            RewardMilestone& milestone) noexcept {
    milestone = {};
    catalog::Definition record{};
    if (!build_data::find_record_definition(recordIndex, record) || record.scoreValue != 0
        || record.completionFlagIndex == catalog::kUnavailableFlagIndex) {
        return false;
    }
    catalog::Objective objective{};
    if (record.objectiveCount != 1 || !catalog::objective(record, 0, objective)
        || objective.completionValue <= 0) {
        return false;
    }
    std::array<catalog::Reward, catalog::kRewardPerRecordCapacity> rewards{};
    std::size_t rewardCount = 0;
    if (!build_data::find_record_rewards(recordIndex, rewards, rewardCount) || rewardCount == 0) {
        return false;
    }
    milestone = {record.completionFlagIndex, objective.valueIndex, objective.completionValue};
    return true;
}

/** Writes the claimed-chapter bars every lore book publishes into both banks. */
void publish_node_progress(Table& table) noexcept {
    node_catalog::for_each(
        &table, [](void* context, const node_catalog::Definition& node) noexcept {
            if (!node.loreBook) {
                return;
            }
            auto& banks = *static_cast<Table*>(context);
            const ChapterCounts counts = chapter_counts(banks, node);
            const bool counterGranted = counter_granted_node(node.definitionIndex);

            std::int32_t parentSlot = -1;
            if (node.parentValueIndex != node_catalog::kUnavailableValueIndex
                && node.parentValueIndex < banks.objectiveValues.size()) {
                // Category gates run last and restore shared zero-valued slots.
                banks.objectiveValues[node.parentValueIndex] =
                    counterGranted ? counts.collected : counts.claimed;
                parentSlot = static_cast<std::int32_t>(node.parentValueIndex);
            }
            // Never overwrite record-objective slots that happen to be named by a node.
            if (node.valueIndex != node_catalog::kUnavailableValueIndex
                && node.valueIndex < catalog::kObjectiveValueIndexBase
                && node.valueIndex < banks.objectiveValues.size()) {
                if (counterGranted) {
                    banks.objectiveValues[node.valueIndex] = counts.collected;
                } else if (counts.cumulative && counts.collected > 0) {
                    // Keep a cumulative collection counter distinct from its claim bar.
                    std::uint16_t counterSlot = node.valueIndex;
                    if (parentSlot == static_cast<std::int32_t>(node.valueIndex)
                        && node.parentValueIndex != node_catalog::kUnavailableValueIndex) {
                        counterSlot = node.parentValueIndex;
                    }
                    if (counterSlot < catalog::kObjectiveValueIndexBase
                        && counterSlot < banks.objectiveValues.size()) {
                        banks.objectiveValues[counterSlot] = counts.collected;
                    }
                }
            }
            if (node.characterValueIndex != node_catalog::kUnavailableValueIndex
                && node.characterValueIndex < banks.characterObjectValues.size()) {
                banks.characterObjectValues[node.characterValueIndex] = counts.allCollected;
            }
            // A character-scoped book's parent bar counts from the character bank too.
            if (node.parentCharacterValueIndex != node_catalog::kUnavailableValueIndex
                && node.parentCharacterValueIndex < banks.characterObjectValues.size()) {
                banks.characterObjectValues[node.parentCharacterValueIndex] =
                    counts.allCollected - counts.claimedParent;
            }
        });
}

/**
 * Publishes the count read by authored node-level reward milestones.
 *
 * These records are a descending reward band at the head of a node's children. Their cards derive
 * presentation from completed siblings, but their action binding reads the objective bank.
 */
void publish_reward_milestones(Table& table) noexcept {
    node_catalog::for_each(
        &table, [](void* context, const node_catalog::Definition& node) noexcept {
            auto& banks = *static_cast<Table*>(context);
            std::array<RewardMilestone, node_catalog::kChildCapacity> milestones{};
            std::size_t milestoneCount = 0;
            for (; milestoneCount < node.childCount; ++milestoneCount) {
                RewardMilestone milestone{};
                if (!resolve_reward_milestone(node.children[milestoneCount], milestone)) {
                    break;
                }
                if (milestoneCount != 0) {
                    const RewardMilestone& previous = milestones[milestoneCount - 1];
                    if (milestone.completionValue >= previous.completionValue
                        || static_cast<std::uint32_t>(milestone.objectiveSlot) + 1U
                               != previous.objectiveSlot) {
                        break;
                    }
                }
                milestones[milestoneCount] = milestone;
            }
            // Two or more descending consecutive rows are the authored milestone-band signature.
            if (milestoneCount < 2) {
                return;
            }
            std::int32_t completedChildren = 0;
            for (std::size_t child = milestoneCount; child < node.childCount; ++child) {
                catalog::Definition record{};
                if (build_data::find_record_definition(node.children[child], record)
                    && record.completionFlagIndex != catalog::kUnavailableFlagIndex
                    && flag_set(banks, record.completionFlagIndex)) {
                    ++completedChildren;
                }
            }
            for (std::size_t index = 0; index < milestoneCount; ++index) {
                const RewardMilestone& milestone = milestones[index];
                if (flag_set(banks, milestone.completionFlagIndex)
                    || milestone.objectiveSlot >= banks.objectiveValues.size()) {
                    continue;
                }
                banks.objectiveValues[milestone.objectiveSlot] =
                    (std::min)(completedChildren, milestone.completionValue);
            }
        });
}

/** Publishes the per-chapter visibility gate for every collected lore chapter. */
void publish_chapter_gates(Table& table) noexcept {
    const std::size_t rows = catalog::count();
    for (std::size_t row = 0; row < rows; ++row) {
        catalog::Definition record{};
        catalog::Objective objective{};
        // A book's parent triumph shares its slot with the book's bar, so only chapters gate.
        if (!catalog::find(static_cast<std::uint16_t>(row), record)
            || record.loreRow == catalog::kUnavailableLoreRow
            || record.completionFlagIndex == catalog::kUnavailableFlagIndex
            || (!flag_set(table, record.completionFlagIndex) && !complete(table, record))
            || !catalog::objective(record, 0, objective)) {
            continue;
        }
        const std::size_t gate = objective.sourceValueIndex;
        if (gate < table.objectiveValues.size()
            && table.objectiveValues[gate] < objective.completionValue) {
            table.objectiveValues[gate] = objective.completionValue;
        }
    }
}

void clear_lore_objectives(Table& table) noexcept;

/** Re-derives every value the claims in the banks imply. Order matters: gates run last. */
void publish_derived(Table& table) noexcept {
    ensure_cache();
    if (g_cacheReady && !investment::store::bootstrap_completed("lore")) {
        // A lore chapter's authored value is not claim state, so the catalogs replace it once.
        clear_lore_objectives(table);
        (void)investment::store::complete_bootstrap("lore");
    }
    (void)build_data::complete_exotic_catalyst_objectives(table.objectiveValues);
    (void)build_data::complete_exotic_catalyst_flags(table.accountFlags);
    (void)node_catalog::apply_visibility(table.accountFlags);
    (void)node_catalog::apply_character_visibility(
        std::as_writable_bytes(std::span(table.characterObjectFlags)));
    publish_node_progress(table);
    publish_reward_milestones(table);
    publish_chapter_gates(table);
    (void)node_catalog::apply_category_gates(table.objectiveValues);
}

/** Zeroes the authored objective values of every record a lore book owns. */
void clear_lore_objectives(Table& table) noexcept {
    node_catalog::for_each(
        &table, [](void* context, const node_catalog::Definition& node) noexcept {
            if (!node.loreBook) {
                return;
            }
            auto& banks = *static_cast<Table*>(context);
            for (std::size_t child = 0; child < node.childCount; ++child) {
                catalog::Definition record{};
                if (!build_data::find_record_definition(node.children[child], record)
                    || record.completionFlagIndex == catalog::kUnavailableFlagIndex) {
                    continue;
                }
                write_objective_run(banks, record, 0, false);
            }
        });
}

void add_score(Table& table, std::int32_t points) noexcept {
    if (catalog::kTriumphScoreValueIndex >= table.objectiveValues.size()) {
        return;
    }
    std::int32_t& slot = table.objectiveValues[catalog::kTriumphScoreValueIndex];
    if (points > 0 && slot > (std::numeric_limits<std::int32_t>::max)() - points) {
        return;
    }
    slot = (std::max)(slot + points, 0);
}

/** One record row and the result of the operation applied to it. */
struct RecordOperation {
    std::uint16_t recordIndex{};
    std::uint32_t definitionHash{};
    bool result{};
};

/** One completion flag and the result of the operation applied to it. */
struct FlagOperation {
    std::uint16_t flagIndex{};
    bool result{};
    ObjectiveAdvance advance{ObjectiveAdvance::unavailable};
};

/**
 * Claims one record: sets its flag, completes its objectives, adds its score, republishes.
 * @param table Live banks; the caller holds the table lock.
 * @return False when the record has no flag or is already claimed.
 */
[[nodiscard]] bool claim_locked(Table& table, std::uint16_t recordIndex) noexcept {
    ensure_cache();
    catalog::Definition record{};
    if (!build_data::find_record_definition(recordIndex, record)
        || record.completionFlagIndex == catalog::kUnavailableFlagIndex
        || record.completionFlagIndex >= table.accountFlags.size()
        || flag_set(table, record.completionFlagIndex)) {
        return false;
    }
    table.accountFlags[record.completionFlagIndex] = kFlagSet;
    write_objective_run(table, record, (std::numeric_limits<std::int32_t>::max)(), true);
    add_score(table, record.scoreValue);
    publish_derived(table);
    return true;
}

/**
 * Grants one lore chapter through its objective run, or through its flag when a counter owns it.
 * @param table Live banks; the caller holds the table lock.
 * @return The reason the grant was refused, or granted.
 */
[[nodiscard]] GrantOutcome grant_locked(Table& table, std::uint16_t recordIndex) noexcept {
    ensure_cache();
    catalog::Definition record{};
    if (!build_data::find_record_definition(recordIndex, record)) {
        return GrantOutcome::recordNotFound;
    }
    if (record.completionFlagIndex == catalog::kUnavailableFlagIndex) {
        return GrantOutcome::noFlag;
    }
    if (record.loreRow == catalog::kUnavailableLoreRow) {
        return GrantOutcome::notAChapter;
    }
    if (flag_set(table, record.completionFlagIndex) || complete(table, record)) {
        return GrantOutcome::alreadyHeld;
    }
    if (counter_granted_flag(record.completionFlagIndex)) {
        // The book's shared counter grants the chapter, so the flag is the only place it fits.
        if (record.completionFlagIndex >= table.accountFlags.size()) {
            return GrantOutcome::refused;
        }
        table.accountFlags[record.completionFlagIndex] = kFlagSet;
    } else {
        write_objective_run(table, record, (std::numeric_limits<std::int32_t>::max)(), true);
    }
    publish_derived(table);
    return GrantOutcome::granted;
}

/**
 * Adds one step to the single-objective record behind a flag.
 * @param table Live banks; the caller holds the table lock.
 * @return Advanced, completed, already held, or unavailable when no such record exists.
 */
[[nodiscard]] ObjectiveAdvance advance_locked(Table& table, std::uint16_t flagIndex) noexcept {
    ensure_cache();
    catalog::Definition record{};
    if (!find_by_flag(flagIndex, record) || record.objectiveCount > 1
        || counter_granted_flag(flagIndex)) {
        return ObjectiveAdvance::unavailable;
    }
    std::array<catalog::Objective, kObjectiveRunCapacity> objectives{};
    const std::size_t slots = record_objectives(record, objectives);
    const std::int32_t completion = slots != 0 ? objectives[0].completionValue : 0;
    const std::size_t first = slots != 0 ? objectives[0].valueIndex : 0;
    if (completion <= 0 || first >= table.objectiveValues.size()) {
        return ObjectiveAdvance::unavailable;
    }
    if (flag_set(table, flagIndex) || complete(table, record)) {
        return ObjectiveAdvance::alreadyHeld;
    }
    const std::int32_t next = (std::min)(table.objectiveValues[first] + 1, completion);
    write_objective_run(table, record, next, true);
    if (next < completion) {
        return ObjectiveAdvance::advanced;
    }
    publish_derived(table);
    return ObjectiveAdvance::completed;
}

/**
 * Redeems the next earned step of an interval record and adds its score. A record with a
 * completion flag sets it once its last step is redeemed.
 * @param table Live banks; the caller holds the table lock.
 * @return False when the record is already complete or its next step is not earned.
 */
[[nodiscard]] bool claim_interval_locked(Table& table, const catalog::Definition& record) noexcept {
    const bool flagged = record.completionFlagIndex != catalog::kUnavailableFlagIndex;
    IntervalLadder ladder{};
    if ((flagged
         && (record.completionFlagIndex >= table.accountFlags.size()
             || flag_set(table, record.completionFlagIndex)))
        || !resolve_ladder(table, record, ladder)) {
        return false;
    }
    std::int32_t& redeemed = table.objectiveValues[record.redeemedCountValueIndex];
    if (redeemed < 0 || redeemed >= record.intervalCount) {
        return false;
    }
    const catalog::Interval& step = ladder.steps[static_cast<std::size_t>(redeemed)];
    if (table.objectiveValues[ladder.progressValueIndex] < step.completionValue) {
        return false;
    }
    add_score(table, static_cast<std::int32_t>(step.score));
    ++redeemed;
    if (flagged && redeemed == record.intervalCount) {
        table.accountFlags[record.completionFlagIndex] = kFlagSet;
        publish_derived(table);
    }
    return true;
}

/**
 * Adds one to the progress every step of an interval record reads, up to its last step.
 * @param table Live banks; the caller holds the table lock.
 * @return Completed when a step becomes earned, already held at the last step or once claimed.
 */
[[nodiscard]] ObjectiveAdvance advance_interval_locked(Table& table,
                                                       const catalog::Definition& record) noexcept {
    IntervalLadder ladder{};
    if (!resolve_ladder(table, record, ladder)) {
        return ObjectiveAdvance::unavailable;
    }
    std::int32_t& progress = table.objectiveValues[ladder.progressValueIndex];
    const auto steps = std::span(ladder.steps).first(record.intervalCount);
    if (flag_set(table, record.completionFlagIndex) || progress >= steps.back().completionValue) {
        return ObjectiveAdvance::alreadyHeld;
    }
    if (progress < 0) {
        return ObjectiveAdvance::unavailable;
    }
    ++progress;
    const bool earned = std::any_of(steps.begin(), steps.end(), [&](const catalog::Interval& step) {
        return step.completionValue == progress;
    });
    return earned ? ObjectiveAdvance::completed : ObjectiveAdvance::advanced;
}

} // namespace

/** Replaces the authored lore values with the ones the seeded claims imply. */
void seed() noexcept {
    mutate(nullptr, [](void*, Table& table) noexcept {
        g_cacheReady = false;
        publish_derived(table);
    });
}

/** Re-derives every bar and gate after a record or node catalog is replaced. */
void republish() noexcept {
    mutate(nullptr, [](void*, Table& table) noexcept {
        g_cacheReady = false;
        publish_derived(table);
    });
}

/** @return True when that record is claimed. */
bool claimed(std::uint16_t flagIndex) noexcept {
    return account_flag_set(flagIndex);
}

/** @return True when that record reads complete while its flag stays clear. */
bool claimable(std::uint16_t flagIndex) noexcept {
    FlagOperation operation{flagIndex, false, ObjectiveAdvance::unavailable};
    const bool saved = mutate(&operation, [](void* context, Table& table) noexcept {
        auto& query = *static_cast<FlagOperation*>(context);
        ensure_cache();
        catalog::Definition record{};
        query.result = !flag_set(table, query.flagIndex) && find_by_flag(query.flagIndex, record)
                       && complete(table, record);
    });
    return saved && operation.result;
}

/** Claims one record: sets its flag, adds its score, and re-derives the bars it feeds. */
bool claim(std::uint16_t recordIndex) noexcept {
    RecordOperation operation{recordIndex, 0, false};
    const bool saved = mutate(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<RecordOperation*>(context);
        request.result = claim_locked(table, request.recordIndex);
    });
    return saved && operation.result;
}

/** Undoes one claim so a refused commit cannot leave the record held. */
void revoke(std::uint16_t recordIndex) noexcept {
    RecordOperation operation{recordIndex, 0, false};
    (void)mutate(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<RecordOperation*>(context);
        ensure_cache();
        catalog::Definition record{};
        if (!build_data::find_record_definition(request.recordIndex, record)
            || record.completionFlagIndex >= table.accountFlags.size()
            || !flag_set(table, record.completionFlagIndex)) {
            return;
        }
        table.accountFlags[record.completionFlagIndex] = kFlagClear;
        add_score(table, -static_cast<std::int32_t>(record.scoreValue));
        publish_derived(table);
    });
}

/** Redeems the next earned step of a record that scores per step. */
bool claim_interval(std::uint16_t recordIndex, std::uint32_t definitionHash) noexcept {
    RecordOperation operation{recordIndex, definitionHash, false};
    const bool saved = mutate(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<RecordOperation*>(context);
        catalog::Definition record{};
        request.result = build_data::find_record_definition(request.recordIndex, record)
                         && record.definitionHash == request.definitionHash
                         && claim_interval_locked(table, record);
    });
    return saved && operation.result;
}

/** @return True when the next interval claim redeems the last step of a flagged record. */
bool interval_claim_completes(std::uint16_t recordIndex) noexcept {
    catalog::Definition record{};
    return build_data::find_record_definition(recordIndex, record)
           && record.completionFlagIndex != catalog::kUnavailableFlagIndex
           && objective_value(record.redeemedCountValueIndex) + 1 == record.intervalCount;
}

/** Adds one to the progress of the interval record with one definition hash. */
ObjectiveAdvance advance_interval(std::uint32_t definitionHash) noexcept {
    struct HashOperation {
        std::uint32_t definitionHash{};
        ObjectiveAdvance advance{ObjectiveAdvance::unavailable};
    } operation{definitionHash, ObjectiveAdvance::unavailable};
    const bool saved = mutate_if_changed(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<HashOperation*>(context);
        ensure_cache();
        catalog::Definition record{};
        if (find_by_hash(request.definitionHash, record)) {
            request.advance = advance_interval_locked(table, record);
        }
        return progress_moved(request.advance);
    });
    return saved ? operation.advance : ObjectiveAdvance::unavailable;
}

/** Marks one lore chapter collected. */
GrantOutcome grant_chapter(std::uint16_t recordIndex) noexcept {
    struct GrantOperation {
        std::uint16_t recordIndex{};
        GrantOutcome outcome{GrantOutcome::refused};
    } operation{recordIndex, GrantOutcome::refused};
    const bool saved = mutate(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<GrantOperation*>(context);
        request.outcome = grant_locked(table, request.recordIndex);
    });
    return saved ? operation.outcome : GrantOutcome::refused;
}

/** Advances the single-objective record that owns one completion flag. */
ObjectiveAdvance advance_objective(std::uint16_t flagIndex) noexcept {
    FlagOperation operation{flagIndex, false, ObjectiveAdvance::unavailable};
    const bool saved = mutate(&operation, [](void* context, Table& table) noexcept {
        auto& request = *static_cast<FlagOperation*>(context);
        request.advance = advance_locked(table, request.flagIndex);
    });
    return saved ? operation.advance : ObjectiveAdvance::unavailable;
}

/** @return Triumph score published in the account value bank. */
std::uint32_t score() noexcept {
    const std::int32_t published = objective_value(catalog::kTriumphScoreValueIndex);
    return published > 0 ? static_cast<std::uint32_t>(published) : 0U;
}

} // namespace sunrise::state::unlocks::records
