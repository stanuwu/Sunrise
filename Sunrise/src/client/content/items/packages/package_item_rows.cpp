#include <array>
#include <span>

#include "../../../../state/build_data/items/catalysts/exotic_catalyst_builder.h"
#include "../../../../state/build_data/items/details/item_detail_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"
#include "package_socket_plug_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace build_details = state::build_data::items::details;
namespace build_items = state::build_data::items;

/** @return True when one extracted detail can join the currently published numeric domains. */
[[nodiscard]] bool publishable_detail(const build_details::Definition& detail) noexcept {
    const std::size_t itemCount = state::build_data::item_definition_count();
    const std::size_t socketListCount = state::build_data::socket_entry_list_count();
    build_items::Definition item{};
    if (!build_details::valid(std::span<const build_details::Definition>{&detail, 1})
        || detail.definitionIndex >= itemCount || detail.socketEntryListIndex >= socketListCount
        || !build_items::find_index(detail.definitionIndex, item)
        || item.bucketId != detail.bucketId) {
        return false;
    }
    for (const std::uint16_t plugIndex : detail.initialPlugIndices) {
        if (plugIndex != build_details::kUnavailableItemIndex && plugIndex >= itemCount) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Walks the located item index table, then publishes every domain that depends on it. */
bool build_item_rows(const reader::Source& source,
                     Storage& storage,
                     const tables::Array& table,
                     std::size_t& rowCount,
                     const char*& reason) noexcept {
    const bool needDefinitions = !state::build_data::item_definitions_ready();
    const bool needDetails = !state::build_data::configured_item_details_ready();
    const bool needSocketPlugs = !state::build_data::socket_plug_rules_ready();
    const bool needCatalysts = !state::build_data::exotic_catalysts_ready();
    const bool needBuckets = !state::build_data::inventory_bucket_descriptors_ready();
    const bool retainDetails = needDetails || needCatalysts;
    const bool needSocketRows = needSocketPlugs || needCatalysts;
    const bool needDetailRows = needDetails || needSocketRows;
    // Bucket equipment slots are derived from this same complete item walk, so a partial retry
    // must still revisit the table even when definitions and detail domains already published.
    const bool needRows = needDefinitions || needDetailRows || needBuckets;
    bool published = !needRows;
    if (retainDetails && storage.details.size() != kDetailCapacity) {
        storage.details.assign(kDetailCapacity, build_details::Definition{});
    }
    if (needCatalysts) {
        storage.catalystCompletionConditions.assign(
            static_cast<std::size_t>(table.count),
            state::build_data::items::catalysts::CompletionCondition{});
        for (std::size_t item = 0; item < storage.catalystCompletionConditions.size(); ++item) {
            storage.catalystCompletionConditions[item].itemDefinitionIndex =
                static_cast<std::uint16_t>(item);
        }
    }
    const bool detailStorageReady = !retainDetails || storage.details.size() == kDetailCapacity;
    const std::span<const std::byte> container{storage.child};
    reason = "rows";
    // The detail closure is gathered during this one walk. Collections can name any installed
    // item row, including profile-owned shaders and modifications, so retain every readable row.
    // The fixed request bitset bounds this to the installed 16-bit item-table domain.
    storage.detailRequests.reset();
    storage.specialPlugCategories.fill(0);
    std::size_t detailCount = 0;
    // One malformed entry is omitted on its own so the rest of the table still publishes. An
    // entry can fail either at its index row or at the definition the row points to, and neither
    // says anything about the entries that follow it.
    std::uint64_t index = 0;
    for (; needRows && index < table.count && rowCount < storage.rows.size(); ++index) {
        tables::IndexRow row{};
        if (!tables::index_row(container, table, index, row)) {
            continue;
        }
        tables::items::Row item{};
        item.definitionHash = row.definitionHash;
        item.definitionIndex = static_cast<std::uint16_t>(index);
        if (!reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)
            || !tables::items::read_definition(std::span<const std::byte>{storage.definition},
                                               item)) {
            continue;
        }
        storage.rows[rowCount++] =
            state::build_data::items::Definition{item.definitionHash,
                                                 item.definitionIndex,
                                                 item.bucketId,
                                                 item.insertionMaterialRequirementSetIndex,
                                                 item.enabledMaterialRequirementSetIndex,
                                                 item.tier,
                                                 item.plugCategoryHash,
                                                 item.rollSetIndex,
                                                 item.linkedPlugIndex};
        if (needSocketRows) {
            storage.specialPlugCategories[item.definitionIndex] =
                special_plug_category(item.plugCategoryHash);
        }
        if (needDetailRows) {
            request(item.definitionIndex, storage.detailRequests);
            append_initial_plugs(item, table.count, storage.detailRequests);
        }
    }
    bool requestsFit = true;
    if (needRows) {
        // Every walked entry either published a row or was skipped, so the count of one
        // follows from the other rather than being tracked alongside them.
        report_row_count(index, rowCount, index - rowCount, index < table.count);
        requestsFit = publish_buckets(storage)
                      && (!needDetailRows
                          || materialize_requests(
                              storage.detailRequests, storage.requestedDetailIndices, detailCount));
        published = rowCount != 0 && requestsFit && detailStorageReady;
    }
    if (published && needDefinitions) {
        published =
            state::build_data::publish_item_definitions(std::span(storage.rows).first(rowCount));
    }
    if (!published) {
        reason = !detailStorageReady ? "detail_storage"
                 : !requestsFit      ? "detail_capacity"
                                     : "publish";
    }
    SocketPlugBuild socketPlugBuild;
    const bool socketStorageReady =
        !needSocketRows
        || socketPlugBuild.prepare(storage.specialPlugCategories,
                                   std::span(storage.rows).first(rowCount));
    if (published && !socketStorageReady) {
        published = false;
        reason = "socket_storage";
    }
    // Every readable installed row is found through the table this pass just published. One
    // malformed row is omitted independently so unrelated Collections categories stay usable.
    if (published && needDetailRows) {
        reason = "details";
        const DetailSource detailSource{
            &source, &storage.scratch, container, table, &storage.definition};
        std::size_t builtDetailCount = 0;
        for (std::size_t slot = 0; slot < detailCount; ++slot) {
            build_details::Definition detail{};
            tables::items::Row item{};
            if (!build_detail(detailSource, storage.requestedDetailIndices[slot], detail, item)
                || !publishable_detail(detail)) {
                report_detail_failure(slot, storage.requestedDetailIndices[slot]);
                continue;
            }
            if (retainDetails) {
                storage.details[builtDetailCount++] = detail;
            }
            if (needCatalysts
                && detail.definitionIndex < storage.catalystCompletionConditions.size()) {
                read_catalyst_completion_condition(
                    std::span<const std::byte>{storage.definition},
                    detail.definitionIndex,
                    storage.catalystCompletionConditions[detail.definitionIndex]);
            }
            if (needSocketRows) {
                (void)socketPlugBuild.append(item,
                                             std::span<const std::byte>{storage.definition},
                                             std::span<const std::byte>{storage.plugSetTable},
                                             table.count);
            }
        }
        if (needDetails) {
            published = state::build_data::publish_configured_item_details(
                std::span<build_details::Definition>{storage.details}.first(builtDetailCount));
            report_detail_count(detailCount, builtDetailCount);
        }
        std::array<state::build_data::items::catalysts::Definition,
                   state::build_data::items::catalysts::kDefinitionCapacity>
            catalystRows{};
        std::size_t catalystCount = 0;
        state::build_data::items::catalysts::Report catalystReport{};
        const state::build_data::items::catalysts::Source catalystSource{
            {},
            std::span(storage.rows).first(rowCount),
            std::span(storage.details).first(builtDetailCount),
            socketPlugBuild.rules(),
            socketPlugBuild.pools(),
            socketPlugBuild.members(),
            storage.catalystCompletionConditions,
            storage.catalystAcquisitionGates,
            storage.catalystObjectiveValues,
        };
        bool catalystBuilt = false;
        if (published && needCatalysts) {
            reason = "exotic_catalysts";
            catalystBuilt = state::build_data::derive_exotic_catalysts(
                catalystSource, catalystRows, catalystCount, catalystReport);
            report_catalyst_catalog(catalystReport, catalystBuilt);
            published = catalystBuilt;
        }
        if (published && needSocketPlugs) {
            const std::size_t rules = socketPlugBuild.rule_count();
            const std::size_t pools = socketPlugBuild.pool_count();
            const std::size_t members = socketPlugBuild.member_count();
            const std::size_t skipped = socketPlugBuild.skipped();
            reason = "socket_plugs";
            published = socketPlugBuild.publish();
            if (published) {
                report_socket_plug_count(rules, pools, members, skipped);
            }
        }
        if (published && needCatalysts && catalystBuilt) {
            reason = "exotic_catalysts";
            published = state::build_data::publish_exotic_catalysts(
                catalystSource, std::span(catalystRows).first(catalystCount));
        }
    }
    // Ability buckets read the socket entry list table again and depend on the detail domain, so
    // they run last. The entry-bucket table never joins the on-disk cache, so a warm boot still
    // has to run this once to fill it in for the session.
    if (published
        && (!state::build_data::ability_buckets_ready()
            || !state::build_data::socket_entry_buckets_ready())) {
        reason = "abilities";
        std::size_t abilityCount = 0;
        std::size_t entryBucketCount = 0;
        const bool built = build_character_abilities(source,
                                                     storage.scratch,
                                                     std::span<const std::byte>{storage.root},
                                                     storage.abilityTable,
                                                     storage.definition,
                                                     storage.abilityPool,
                                                     storage.abilityRows,
                                                     abilityCount,
                                                     storage.entryBucketRows,
                                                     entryBucketCount);
        published = built
                    && state::build_data::publish_ability_buckets(
                        std::span(storage.abilityRows).first(abilityCount))
                    && state::build_data::publish_socket_entry_buckets(
                        std::span(storage.entryBucketRows).first(entryBucketCount));
        if (built) {
            report_ability_count(abilityCount);
        }
    }
    return published && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready()
           && state::build_data::exotic_catalysts_ready()
           && state::build_data::ability_buckets_ready();
}

} // namespace sunrise::client::content::items::packages
