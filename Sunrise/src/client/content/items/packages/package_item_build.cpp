#include <Windows.h>

#include <array>
#include <atomic>
#include <span>
#include <vector>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
#include "../../../../state/account/inventory/synthesizer_tier.h"
#include "../../../../state/build_data/activities/activity_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/vendors/vendor_gate_catalog.h"
#include "../../activity/activity_catalog_build.h"
#include "../../activity/entity_position_profile_build.h"
#include "../../activity/public_event_schedule_build.h"
#include "../../hash_names/hash_name_build.h"
#include "../../scenarios/scenario_build.h"
#include "../../spawn_sets/spawn_set_build.h"
#include "../../vendors/vendor_build.h"
#include "build.h"
#include "internal.h"
#include "package_objective_build.h"
#include "package_reward_build.h"
#include "package_socket_plug_build.h"

namespace sunrise::client::content::items::packages {
namespace {

/** The optional schedule is published or all candidates in the sealed catalog are unsupported. */
std::atomic_bool g_scheduleSettled{};

/** @return True when every item and investment-root domain is published. */
[[nodiscard]] bool root_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
           && state::build_data::collectible_definitions_ready()
           && state::build_data::material_requirement_sets_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::socket_entry_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::season_pass_ready() && reward_definitions_settled()
           && state::build_data::repeatable_bounties_ready()
           && state::build_data::record_definitions_ready()
           && state::build_data::node_definitions_ready()
           && state::build_data::sobject_definitions_ready()
           && state::build_data::investment_constants_ready()
           && state::build_data::starter_sets_ready() && exotic_catalysts_settled()
           && pursuit_catalog_settled() && plug_insertion_rules_settled()
           && combat_taxonomy_settled();
}

/**
 * Reads the items the client's grant refuses: one row per item, its item-table index first.
 * @return False when the installed list cannot be read; a later pass retries it.
 */
[[nodiscard]] bool read_restricted_grants(const reader::Source& source, Storage& storage) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t tableClass = 0;
    std::vector<std::byte> blob{};
    tables::Array rows{};
    if (!tables::slot_tag(
            std::span<const std::byte>{storage.root}, tables::kRestrictedGrantTableSlot, tag)) {
        return false;
    }
    storage.restrictedGrants.assign(state::build_data::items::kDefinitionCapacity, false);
    // A build without the list restricts nothing.
    if (tag == 0) {
        return true;
    }
    if (!reader::read_tag(source, storage.scratch, tag, blob, tableClass)
        || tableClass != tables::kRestrictedGrantTableClass
        || !tables::read_array(std::span<const std::byte>{blob},
                               tables::kRestrictedGrantArrayDescriptor,
                               tables::kRestrictedGrantElementClass,
                               tables::kRestrictedGrantStride,
                               rows)) {
        return false;
    }
    for (std::size_t row = 0; row < rows.count; ++row) {
        const std::size_t at = rows.dataOffset + row * tables::kRestrictedGrantStride;
        std::uint16_t itemIndex = 0;
        if (!tables::read(std::span<const std::byte>{blob},
                          at + tables::kRestrictedGrantItemIndexOffset,
                          itemIndex)
            || itemIndex >= storage.restrictedGrants.size()) {
            return false;
        }
        storage.restrictedGrants[itemIndex] = true;
    }
    return true;
}

} // namespace

/** @return True when every domain owned by the package pass is published. */
bool ready() noexcept {
    return root_domains_ready() && state::build_data::scenario_layouts_ready()
           && state::build_data::spawn_sets_ready() && state::build_data::hash_names_ready()
           && state::build_data::vendor_catalog_ready() && state::build_data::vendors::gates_ready()
           && g_scheduleSettled.load() && content::activity::entity_position_profiles::ready()
           && (state::build_data::activities::ready()
               || state::build_data::activities::extraction_failed());
}

/**
 * Publishes missing package domains while retaining completed domains for later calls.
 * @return True when every owned domain is ready; false leaves unfinished work for another call.
 */
bool build() noexcept {
    static Storage storage{};
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys)) {
        report(0, "keys");
        return false;
    }
    std::size_t rowCount = 0;
    const char* reason = "directory";
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report(0, reason);
        return false;
    }
    // The destination layouts and the spawn sets share this pass's directory, keys, and block
    // storage. Both are independent of the item table, so a failure here leaves it alone. The
    // vendor catalog binds its availability conditions through the root below.
    {
        const reader::Source packageSource{directory.chars.data(), &keys};
        // Process-local launcher data is extracted even when persistent build data was loaded.
        (void)content::activity::build_catalog(packageSource, storage.scratch);
        if (!g_scheduleSettled.load()
            && content::activity::build_public_event_schedule(packageSource, storage.scratch)) {
            g_scheduleSettled.store(true);
        }
        (void)content::activity::entity_position_profiles::build(packageSource, storage.scratch);
        (void)content::scenarios::build(packageSource, storage.scratch);
        (void)content::spawn_sets::build(packageSource, storage.scratch);
        (void)content::hash_names::build(packageSource, storage.scratch);
        if (ready()) {
            SecureZeroMemory(&keys, sizeof keys);
            return true;
        }
    }
    if (root_domains_ready() && state::build_data::vendor_catalog_ready()
        && state::build_data::vendors::gates_ready()) {
        SecureZeroMemory(&keys, sizeof keys);
        return ready();
    }
    reason = "tag";
    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    if (investment_globals_tags(candidates, candidateCount)) {
        const reader::Source source{directory.chars.data(), &keys};
        tables::Array table{};
        bool located = false;
        reason = "read";
        for (std::size_t candidate = 0; candidate < candidateCount && !located; ++candidate) {
            if (!reader::read_tag(
                    source, storage.scratch, candidates[candidate], storage.container)) {
                continue;
            }
            // Fixed navigation: globals child zero is the investment root, whose slot holds the
            // item table, whose array descriptor sits at a fixed offset.
            std::uint32_t rootTag = 0;
            std::uint32_t rootClass = 0;
            std::uint32_t tableTag = 0;
            reason = "root";
            if (!tables::child_tag(std::span<const std::byte>{storage.container},
                                   tables::kInvestmentRootChild,
                                   rootTag)
                || rootTag == 0 || tables::package_of(rootTag) == tables::kAbsentPackageId
                || !reader::read_tag(source, storage.scratch, rootTag, storage.child, rootClass)
                || rootClass != tables::kInvestmentRootClass) {
                continue;
            }
            // The same root names the bucket and socket-list tables.
            storage.root = storage.child;
            // Quest, record, node, season, catalyst, pursuit, material, plug-rule and vendor reads
            // share these maps.
            if (!state::build_data::item_definitions_ready()
                || !state::build_data::vendor_catalog_ready() || !plug_insertion_rules_settled()
                || !state::build_data::material_requirement_sets_ready()
                || !state::build_data::record_definitions_ready()
                || !state::build_data::node_definitions_ready()
                || !state::build_data::season_pass_ready() || !reward_definitions_settled()
                || !exotic_catalysts_settled() || !pursuit_catalog_settled()
                || !state::build_data::vendors::gates_ready()) {
                reason = "unlock_maps";
                if (!read_unlock_slot_maps(
                        source, storage, std::span<const std::byte>{storage.root})) {
                    continue;
                }
            }
            // Objective programs, sale rows, reward pools, pass rows, plug insertion rules and
            // pursuit reward rows bind through the condition tables, so one load serves the pass.
            RewardConditions& conditions = storage.rewardBuild.conditions;
            if (!pursuit_catalog_settled() || !state::build_data::vendor_catalog_ready()
                || !reward_definitions_settled() || !state::build_data::season_pass_ready()
                || !plug_insertion_rules_settled()) {
                (void)conditions.load(source,
                                      storage.scratch,
                                      std::span<const std::byte>{storage.root},
                                      storage.slotMaps);
            }
            // The objective table settles or retries on its own, so it never holds up the others.
            if (!pursuit_catalog_settled()) {
                (void)build_objectives(
                    source, storage.scratch, std::span<const std::byte>{storage.root}, conditions);
            }
            // Sale rows bind their availability through the condition tables. A refused reward
            // graph still publishes the catalog: a row whose conditions cannot bind is not sold.
            if (!state::build_data::vendor_catalog_ready()
                && (conditions.loaded() || state::build_data::reward_definitions_unsupported())) {
                (void)content::vendors::build(source, storage.scratch, conditions);
            }
            // Item rows read the flags the plug sets gate their members on.
            if (!state::build_data::socket_plug_rules_ready()
                || !state::build_data::item_definitions_ready()) {
                std::uint32_t plugSetTag = 0;
                tables::Array plugSets{};
                reason = "plug_sets";
                if (!tables::slot_tag(std::span<const std::byte>{storage.root},
                                      tables::kPlugSetTableSlot,
                                      plugSetTag)
                    || plugSetTag == 0
                    || !reader::read_tag(source, storage.scratch, plugSetTag, storage.plugSetTable)
                    || !tables::find_array_at(std::span<const std::byte>{storage.plugSetTable},
                                              offsetof(generated::C80802cb6, table),
                                              plugSets)) {
                    continue;
                }
            }
            if (!exotic_catalysts_settled()) {
                reason = "catalyst_gates";
                if (!read_catalyst_acquisition_gates(source,
                                                     storage.scratch,
                                                     std::span<const std::byte>{storage.root},
                                                     storage.child,
                                                     storage.catalystAcquisitionGates)
                    || !read_catalyst_objective_values(source,
                                                       storage.scratch,
                                                       std::span<const std::byte>{storage.root},
                                                       storage.child,
                                                       storage.catalystObjectiveValues)) {
                    continue;
                }
            }
            reason = "buckets";
            if (!build_buckets(source, storage, std::span<const std::byte>{storage.root})) {
                continue;
            }
            (void)build_socket_entry_lists(
                source, storage, std::span<const std::byte>{storage.root});
            if (!state::build_data::progression_definitions_ready()) {
                std::size_t progressionCount = 0;
                if (build_progressions(source,
                                       storage.scratch,
                                       std::span<const std::byte>{storage.root},
                                       storage.child,
                                       storage.progressionRows,
                                       progressionCount,
                                       storage.progressionSteps,
                                       storage.progressionStepCount)) {
                    (void)state::build_data::publish_progression_definitions(
                        std::span(storage.progressionRows).first(progressionCount),
                        std::span(storage.progressionSteps).first(storage.progressionStepCount));
                }
            }
            // The pools are read again while any domain that binds through the condition tables
            // is open.
            if (!reward_definitions_settled() || !state::build_data::season_pass_ready()
                || !plug_insertion_rules_settled() || !pursuit_catalog_settled()) {
                (void)storage.rewardBuild.load(source, storage.scratch, storage.root);
            }
            // A refused reward graph still publishes the pass. A row whose condition cannot bind
            // stays unavailable, and every claim refuses without the graph.
            if (!state::build_data::season_pass_ready()
                && (conditions.loaded() || state::build_data::reward_definitions_unsupported())
                && build_season_pass(source, storage, std::span<const std::byte>{storage.root})
                && !state::build_data::publish_season_pass(
                    std::span(storage.seasonPassRewards).first(storage.seasonPassRewardCount))) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 "ev=pkg stage=season_pass result=fail reason=publish");
            }
            // Records are read before nodes: a node's lore-book flag and its parent bar come
            // from the records it owns, so the record rows must already be in pass storage.
            if (!state::build_data::record_definitions_ready()
                || !state::build_data::node_definitions_ready()) {
                const bool builtRecords =
                    build_records(source, storage, std::span<const std::byte>{storage.root});
                if (builtRecords && !state::build_data::record_definitions_ready()) {
                    (void)state::build_data::publish_record_definitions(
                        std::span(storage.recordRows).first(storage.recordCount),
                        std::span(storage.recordObjectives).first(storage.recordObjectiveCount),
                        std::span(storage.recordIntervals).first(storage.recordIntervalCount),
                        std::span(storage.recordRewards).first(storage.recordRewardCount));
                }
                if (builtRecords && !state::build_data::node_definitions_ready()
                    && build_nodes(source, storage, std::span<const std::byte>{storage.root})) {
                    (void)state::build_data::publish_node_definitions(
                        std::span(storage.nodeRows).first(storage.nodeCount));
                }
            }
            if (!state::build_data::investment_constants_ready()) {
                state::build_data::constants::InvestmentConstants extracted{};
                if (read_investment_constants(source,
                                              storage.scratch,
                                              std::span<const std::byte>{storage.root},
                                              storage.child,
                                              extracted)) {
                    // An unbound row leaves the Synthesizer tier unpublished, not the constants.
                    if (!read_character_value_row(
                            source,
                            storage.scratch,
                            std::span<const std::byte>{storage.root},
                            storage.child,
                            state::account::inventory::kSynthesizerTierValueHash,
                            extracted.synthesizerTierValueRow)) {
                        core::log::write(core::log::Channel::client,
                                         core::log::Level::warn,
                                         "ev=pkg stage=synthesizer_tier_row result=unbound");
                    }
                    (void)state::build_data::publish_investment_constants(extracted);
                }
            }
            if (!combat_taxonomy_settled()) {
                (void)build_combat_taxonomy(source,
                                            storage.scratch,
                                            std::span<const std::byte>{storage.root},
                                            storage.child);
            }
            if (!state::build_data::starter_sets_ready()) {
                (void)build_starter_sets(source,
                                         storage.scratch,
                                         std::span<const std::byte>{storage.root},
                                         storage.child);
            }
            // Only item rows carry the list, so a pass that has them published skips the read.
            reason = "restricted_grants";
            if (!state::build_data::item_definitions_ready()
                && !read_restricted_grants(source, storage)) {
                continue;
            }
            reason = "slot";
            if (!tables::slot_tag(
                    std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
                || tableTag == 0
                || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
                continue;
            }
            reason = "table";
            located = tables::find_array_at(std::span<const std::byte>{storage.child},
                                            offsetof(generated::ItemIndexTable, table),
                                            table)
                      && table.elementClass == tables::kItemIndexTableClass;
        }
        const bool rowsBuilt = located && !root_domains_ready()
                               && build_item_rows(source, storage, table, rowCount, reason);
        if (rowsBuilt) {
            if (!build_material_requirements(
                    source, storage, std::span<const std::byte>{storage.root}, table.count)) {
                reason = "materials";
            } else if (!build_collectibles(source,
                                           storage,
                                           std::span<const std::byte>{storage.root},
                                           table.count)) {
                reason = "collectibles";
            } else if (!state::build_data::repeatable_bounties_ready()
                       && build_bounties(source, storage, rowCount)) {
                (void)state::build_data::publish_repeatable_bounties(
                    std::span(storage.bountyRows).first(storage.bountyCount));
            }
        }
        if (located && root_domains_ready() && !state::build_data::vendors::gates_ready()) {
            reason = "vendor_gates";
            const content::vendors::GateMaps maps{storage.slotMaps.accountFlag,
                                                  storage.slotMaps.profileFlag,
                                                  storage.slotMaps.characterFlag,
                                                  storage.slotMaps.accountValue,
                                                  storage.slotMaps.characterValue};
            (void)content::vendors::build_gates(source, storage.scratch, maps);
        }
    }
    SecureZeroMemory(&keys, sizeof keys);
    const bool complete = ready();
    const bool itemDomainsReady = root_domains_ready();
    if (complete) {
        // Nothing reads a package again until the next boot, so this reader's files go back now.
        reader::close_files(storage.scratch);
    }
    // Scenario, spawn-set, and hash-name extraction advance over later refresh slices and report
    // their own progress. Do not mislabel one of those pending domains as the last item substage.
    report(itemDomainsReady ? state::build_data::item_definition_count() : 0, reason);
    return complete;
}

} // namespace sunrise::client::content::items::packages
