#include <array>
#include <span>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808077.h"
#include "../../../../middleware/content/packages/tables/quest_initialization_reader.h"
#include "../../../../state/build_data/items/catalysts/exotic_catalyst_builder.h"
#include "../../../../state/build_data/items/details/item_detail_catalog.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"
#include "package_socket_plug_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace build_details = state::build_data::items::details;
namespace build_items = state::build_data::items;
namespace pursuits = state::build_data::pursuits;
namespace insertion_rules = state::build_data::items::insertion_rules;

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

/** The plug-set flags read so far, and the items any set has offered yet. */
struct PlugSetFlagScan {
    std::vector<std::uint16_t>& flags;
    std::vector<bool>& offered;
    const SlotMap& accountFlags;
};

/** Keeps an item's flag while every set offering it requires the same one, else unmaps it. */
void record_plug_set_flag(void* context,
                          std::uint32_t itemDefinitionIndex,
                          std::int16_t flagSlot) noexcept {
    auto& scan = *static_cast<PlugSetFlagScan*>(context);
    if (itemDefinitionIndex >= scan.flags.size()) {
        return;
    }
    const std::uint16_t row = bank_index(scan.accountFlags, flagSlot);
    std::uint16_t& flag = scan.flags[itemDefinitionIndex];
    flag = !scan.offered[itemDefinitionIndex] || flag == row ? row : kUnmappedSlot;
    scan.offered[itemDefinitionIndex] = true;
}

/**
 * Resolves, per item, the account flag row the shared plug sets offering it require set, over
 * rows already unmapped. A table that does not parse leaves every item unmapped, which refuses
 * whatever needs the flag.
 */
void read_plug_set_flags(Storage& storage) noexcept {
    const std::size_t itemCount = storage.plugSetFlagIndices.size();
    std::vector<bool> offered(itemCount, false);
    PlugSetFlagScan scan{storage.plugSetFlagIndices, offered, storage.slotMaps.accountFlag};
    if (!tables::items::visit_plug_set_flags(
            std::span<const std::byte>{storage.plugSetTable}, record_plug_set_flag, &scan)) {
        storage.plugSetFlagIndices.assign(itemCount, kUnmappedSlot);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=pkg stage=plug_set_flags result=fail");
    }
}

/**
 * Binds the insertion rules one plug declares. A list that does not read, bind or fit is kept as
 * an unreadable row, which refuses the plug; a plug with no rules adds no row.
 * @return False when the rows are full or a bank allocation failed, which retries the walk.
 */
[[nodiscard]] bool read_insertion_rules(Storage& storage, const tables::items::Row& item) noexcept {
    std::size_t at = 0;
    if (!tables::items::find_insertion_rules(std::span<const std::byte>{storage.definition}, at)) {
        return true;
    }
    insertion_rules::Rule rule{};
    rule.plugDefinitionIndex = item.definitionIndex;
    std::size_t count = 0;
    BankFailure failure = BankFailure::none;
    rule.readable = storage.rewardBuild.conditions.read_list(
        std::span<const std::byte>{storage.definition},
        at,
        rule.condition,
        count,
        failure,
        generated::C808077e6::kClass);
    if (failure == BankFailure::storage) {
        return false;
    }
    if (rule.readable && count == 0) {
        return true;
    }
    if (rule.readable) {
        rule.conditionCount = static_cast<std::uint8_t>(count);
        rule.requiredFlagIndex = bank_index(storage.slotMaps.accountFlag, item.insertionFlagSlot);
    }
    if (storage.insertionRuleCount == storage.insertionRules.size()) {
        return false;
    }
    storage.insertionRules[storage.insertionRuleCount++] = rule;
    return true;
}

} // namespace

/** @return True when the catalyst catalog is published or cannot exist on this executable. */
bool exotic_catalysts_settled() noexcept {
    return state::build_data::exotic_catalysts_ready()
           || state::build_data::exotic_catalysts_unsupported();
}

/**
 * Publishes missing item domains from the located index table and its definitions.
 * @param source Borrowed package source.
 * @param storage Pass storage holding the table in child, root data, maps, and output rows.
 * @param table Located item index array within storage.child.
 * @param rowCount Starts at zero; receives the rows retained even if publication fails.
 * @param reason Receives the last stage reached or its failure reason.
 * @return True when required non-reward item domains are ready. Failure may retain prior results.
 */
bool build_item_rows(const reader::Source& source,
                     Storage& storage,
                     const tables::Array& table,
                     std::size_t& rowCount,
                     const char*& reason) noexcept {
    // Contracts are checked against the objective table, so a walk without it would be wasted.
    const bool pursuitStorageReady =
        !pursuit_catalog_settled() && state::build_data::objective_definitions_ready();
    if (pursuitStorageReady) {
        storage.pursuitBuild.begin();
    }
    const bool needRewards = !reward_definitions_settled();
    const bool rewardStorageReady =
        needRewards && storage.rewardBuild.begin_items(static_cast<std::size_t>(table.count));
    const bool needDefinitions = !state::build_data::item_definitions_ready();
    const bool needDetails = !state::build_data::configured_item_details_ready();
    const bool needSocketPlugs = !state::build_data::socket_plug_rules_ready();
    const bool needCatalysts = !exotic_catalysts_settled();
    const bool needBuckets = !state::build_data::inventory_bucket_descriptors_ready();
    // Insertion rules bind through the reward condition tables, which load before this walk.
    const bool needInsertionRules = !state::build_data::plug_insertion_rules_ready()
                                    && storage.rewardBuild.conditions.loaded();
    const bool retainDetails = needDetails || needCatalysts;
    const bool needSocketRows = needSocketPlugs || needCatalysts;
    const bool needDetailRows = needDetails || needSocketRows;
    // Bucket equipment slots are derived from this same complete item walk, so a partial retry
    // must still revisit the table even when definitions and detail domains already published.
    const bool needRows = needDefinitions || needDetailRows || needBuckets || rewardStorageReady
                          || pursuitStorageReady || needInsertionRules;
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
    storage.plugSetFlagIndices.assign(static_cast<std::size_t>(table.count), kUnmappedSlot);
    if (needDefinitions) {
        read_plug_set_flags(storage);
    }
    storage.insertionRuleCount = 0;
    if (needInsertionRules) {
        storage.insertionRules.assign(insertion_rules::kRuleCapacity, insertion_rules::Rule{});
    }
    bool insertionRulesRead = needInsertionRules;
    std::size_t detailCount = 0;
    // One malformed entry is omitted on its own so the rest of the table still publishes. An
    // entry can fail either at its index row or at the definition the row points to, and neither
    // says anything about the entries that follow it.
    std::uint64_t index = 0;
    for (; needRows && index < table.count && rowCount < storage.rows.size(); ++index) {
        tables::IndexRow row{};
        if (!tables::index_row(container, table, index, row)) {
            if (pursuitStorageReady) {
                storage.pursuitBuild.unread(static_cast<std::uint16_t>(index));
            }
            continue;
        }
        tables::items::Row item{};
        item.definitionHash = row.definitionHash;
        item.definitionIndex = static_cast<std::uint16_t>(index);
        std::uint32_t itemClass = 0;
        if (!reader::read_tag(
                source, storage.scratch, row.targetTag, storage.definition, itemClass)) {
            // The published catalog read this row before, so its reward and contract wait for a
            // retry. A row the catalog lacks stays out of the rewards and contracts as well.
            state::build_data::items::Definition cataloged{};
            const bool retryable =
                state::build_data::find_item_definition_index(item.definitionIndex, cataloged);
            if (rewardStorageReady && retryable) {
                storage.rewardBuild.item_unread(item.definitionIndex);
            }
            if (pursuitStorageReady && retryable) {
                storage.pursuitBuild.read_failed(item.definitionIndex);
            } else if (pursuitStorageReady) {
                storage.pursuitBuild.unread(item.definitionIndex);
            }
            continue;
        }
        if (!tables::items::read_definition(std::span<const std::byte>{storage.definition}, item)) {
            if (pursuitStorageReady) {
                storage.pursuitBuild.unread(item.definitionIndex);
            }
            continue;
        }
        if (rewardStorageReady && itemClass == tables::kItemDefinitionClass) {
            storage.rewardBuild.item(item.definitionIndex, item.definitionHash, storage.definition);
        }
        if (insertionRulesRead && itemClass == tables::kItemDefinitionClass) {
            insertionRulesRead = read_insertion_rules(storage, item);
        }
        const std::uint32_t plugCategoryHash =
            corrected_plug_category(item.definitionHash, item.plugCategoryHash);
        build_items::QuestInitialization quest{};
        pursuits::QuestStep questStep{};
        bool questOwnerRead = true;
        const auto parentIndex = tables::items::quest_parent(storage.definition);
        if ((needDefinitions || pursuitStorageReady) && itemClass == tables::kItemDefinitionClass
            && parentIndex < table.count) {
            std::span<const std::byte> parent = storage.definition;
            tables::IndexRow parentRow{};
            std::uint32_t parentClass = itemClass;
            const bool parentIndexed =
                parentIndex == item.definitionIndex
                || tables::index_row(container, table, parentIndex, parentRow);
            const bool parentReady = parentIndexed
                                     && (parentIndex == item.definitionIndex
                                         || reader::read_tag(source,
                                                             storage.scratch,
                                                             parentRow.targetTag,
                                                             storage.questParentDefinition,
                                                             parentClass));
            // Like an item's own row, an owner the catalog read before is retried after a failed
            // read; one it lacks leaves the set unsupported.
            state::build_data::items::Definition cataloged{};
            questOwnerRead =
                parentReady || !parentIndexed
                || !state::build_data::find_item_definition_index(parentIndex, cataloged);
            if (parentIndex != item.definitionIndex) {
                parent = storage.questParentDefinition;
            }
            const bool parentUsable = parentReady && parentClass == tables::kItemDefinitionClass;
            if (needDefinitions && parentUsable) {
                quest =
                    tables::items::read_quest_initialization(storage.definition,
                                                             item.definitionIndex,
                                                             parent,
                                                             static_cast<std::size_t>(table.count),
                                                             storage.questValueMap);
            }
            if (pursuitStorageReady && parentUsable) {
                questStep = tables::items::read_quest_step(storage.definition,
                                                           item.definitionIndex,
                                                           parent,
                                                           static_cast<std::size_t>(table.count),
                                                           storage.questValueMap);
            } else if (pursuitStorageReady) {
                questStep.membership = pursuits::QuestMembership::unsupported;
            }
        } else if (parentIndex != tables::items::kUnavailableQuestParent) {
            questStep.membership = pursuits::QuestMembership::unsupported;
        }
        if (pursuitStorageReady && itemClass != tables::kItemDefinitionClass) {
            storage.pursuitBuild.unread(item.definitionIndex);
        } else if (pursuitStorageReady
                   && (!questOwnerRead || !storage.pursuitBuild.item(item, questStep))) {
            storage.pursuitBuild.read_failed(item.definitionIndex);
        }
        storage.rows[rowCount++] =
            state::build_data::items::Definition{item.definitionHash,
                                                 item.definitionIndex,
                                                 item.bucketId,
                                                 item.insertionMaterialRequirementSetIndex,
                                                 item.enabledMaterialRequirementSetIndex,
                                                 item.tier,
                                                 plugCategoryHash,
                                                 item.rollSetIndex,
                                                 item.linkedPlugIndex,
                                                 quest,
                                                 item.recoveryBucketId,
                                                 item.quietAcquisition,
                                                 item.toastlessAcquisition,
                                                 item.stackUniqueLabel,
                                                 storage.restrictedGrants[item.definitionIndex],
                                                 bank_index(storage.slotMaps.accountValue,
                                                            item.quantityValueSlot),
                                                 storage.plugSetFlagIndices[item.definitionIndex]};
        if (needSocketRows) {
            storage.specialPlugCategories[item.definitionIndex] =
                special_plug_category(plugCategoryHash);
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
    // A reward refusal is reported by RewardBuild and does not block the other item domains.
    // Rewards publish only beside the item catalog and at its row count, so both omit the same
    // unread rows.
    const bool rewardsPublished =
        !needRewards
        || (rewardStorageReady && state::build_data::item_definitions_ready()
            && storage.rewardBuild.publish(state::build_data::item_definition_count()));
    // Pursuit contracts settle on their own and never block the other item domains.
    if (pursuitStorageReady) {
        storage.pursuitBuild.publish();
    }
    if (!published) {
        reason = !detailStorageReady ? "detail_storage"
                 : !requestsFit      ? "detail_capacity"
                                     : "publish";
    }
    // A plug whose rules failed to read keeps its unreadable row; only a full or failed walk
    // leaves the rules for another pass.
    if (published && needInsertionRules) {
        published = insertionRulesRead && index == table.count
                    && state::build_data::publish_plug_insertion_rules(
                        std::span(storage.insertionRules).first(storage.insertionRuleCount));
        if (!published) {
            reason = "insertion_rules";
        }
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
            storage.slotMaps.accountFlag,
        };
        bool catalystBuilt = false;
        if (published && needCatalysts) {
            reason = "exotic_catalysts";
            catalystBuilt = state::build_data::derive_exotic_catalysts(
                catalystSource, catalystRows, catalystCount, catalystReport);
            report_catalyst_catalog(catalystReport, catalystBuilt);
            // An unsupported executable settles the domain: retrying repeats the whole item walk
            // on every refresh slice and reaches the same verdict.
            published = catalystBuilt || state::build_data::exotic_catalysts_unsupported();
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
    // Named last; later stages overwrite the reason as they start.
    if (published && !rewardsPublished) {
        reason = "rewards";
    }
    return published && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready() && exotic_catalysts_settled()
           && state::build_data::ability_buckets_ready()
           && state::build_data::plug_insertion_rules_ready();
}

} // namespace sunrise::client::content::items::packages
