#include <Windows.h>

#include <array>
#include <span>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/activities/activity_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "../../activity/activity_catalog_build.h"
#include "../../activity/entity_position_profile_build.h"
#include "../../hash_names/hash_name_build.h"
#include "../../scenarios/scenario_build.h"
#include "../../spawn_sets/spawn_set_build.h"
#include "../../vendors/vendor_build.h"
#include "build.h"
#include "internal.h"
#include "package_socket_plug_build.h"

namespace sunrise::client::content::items::packages {
namespace {

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
           && state::build_data::season_pass_ready()
           && state::build_data::repeatable_bounties_ready()
           && state::build_data::record_definitions_ready()
           && state::build_data::node_definitions_ready()
           && state::build_data::sobject_definitions_ready()
           && state::build_data::investment_constants_ready() && exotic_catalysts_settled();
}

} // namespace

/** @return True when every domain owned by the package pass is published. */
bool ready() noexcept {
    return root_domains_ready() && state::build_data::scenario_layouts_ready()
           && state::build_data::spawn_sets_ready() && state::build_data::hash_names_ready()
           && state::build_data::vendor_catalog_ready()
           && content::activity::entity_position_profiles::ready()
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
    // storage. Both are independent of the item table, so a failure here leaves it alone.
    {
        const reader::Source packageSource{directory.chars.data(), &keys};
        // Process-local launcher data is extracted even when persistent build data was loaded.
        (void)content::activity::build_catalog(packageSource, storage.scratch);
        (void)content::activity::entity_position_profiles::build(packageSource, storage.scratch);
        (void)content::scenarios::build(packageSource, storage.scratch);
        (void)content::spawn_sets::build(packageSource, storage.scratch);
        (void)content::hash_names::build(packageSource, storage.scratch);
        (void)content::vendors::build(packageSource, storage.scratch);
        if (ready()) {
            SecureZeroMemory(&keys, sizeof keys);
            return true;
        }
    }
    if (root_domains_ready()) {
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
            // Quest, record, node, season and catalyst reads share these unlock maps.
            if (!state::build_data::item_definitions_ready()
                || !state::build_data::record_definitions_ready()
                || !state::build_data::node_definitions_ready()
                || !state::build_data::season_pass_ready() || !exotic_catalysts_settled()) {
                reason = "unlock_maps";
                if (!read_unlock_slot_maps(
                        source, storage, std::span<const std::byte>{storage.root})) {
                    continue;
                }
            }
            if (!state::build_data::socket_plug_rules_ready()) {
                std::uint32_t plugSetTag = 0;
                tables::Array plugSets{};
                reason = "plug_sets";
                if (!tables::slot_tag(std::span<const std::byte>{storage.root},
                                      tables::kPlugSetTableSlot,
                                      plugSetTag)
                    || plugSetTag == 0
                    || !reader::read_tag(source, storage.scratch, plugSetTag, storage.plugSetTable)
                    || !tables::find_array_at(std::span<const std::byte>{storage.plugSetTable},
                                              tables::kTableArrayDescriptor,
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
            if (!state::build_data::season_pass_ready()
                && build_season_pass(source, storage, std::span<const std::byte>{storage.root})) {
                (void)state::build_data::publish_season_pass(
                    std::span(storage.seasonPassRewards).first(storage.seasonPassRewardCount),
                    std::span(storage.seasonPassPackages).first(storage.seasonPassPackageCount));
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
                    (void)state::build_data::publish_investment_constants(extracted);
                }
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
                                            tables::kTableArrayDescriptor,
                                            table)
                      && table.elementClass == tables::kItemIndexTableClass;
        }
        if (located && build_item_rows(source, storage, table, rowCount, reason)) {
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
