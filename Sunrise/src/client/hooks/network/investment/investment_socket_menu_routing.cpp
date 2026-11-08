#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../middleware/content/packages/reader/layout.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80802c.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80802d.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80805b.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808077.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80807b.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_80807d.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/items/socket_plugs/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/content/content_catalog.h"
#include "../../../content/handles/handle_resolver.h"
#include "../../../content/investment/internal.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game/content.h"
#include "internal.h"
#include "socket_menu_low_arena.h"
#include "socket_menu_native_array.h"
#include "socket_row_relocation.h"

namespace sunrise::client::hooks::network::investment {
namespace {

namespace content_handles = client::content::handles;
namespace content_investment = client::content::investment;

/** FNV-1 name hash shared by the installed investment-globals candidates. */
constexpr std::uint32_t kInvestmentGlobalsNameHash = 0x6F7125CBU;
constexpr std::size_t kBootstrapCandidateCapacity = 64;

namespace socket_plugs = state::build_data::items::socket_plugs;

/** Exact installed item identities used to validate a candidate investment root. */
using socket_plugs::kArrivalsLegModHashes;
using socket_plugs::kArrivalsLegReferenceHash;

namespace generated = middleware::content::packages::schema::generated;

/** Root tag the globals name, and the two root slots this patch reads. */
constexpr std::size_t kGlobalsRootTagOffset =
    offsetof(generated::InvestmentGlobals, investmentRoot);
constexpr std::size_t kItemTableTagOffset = offsetof(generated::InvestmentRoot, slot48ItemIndex);
constexpr std::size_t kPlugSetTableTagOffset = offsetof(generated::InvestmentRoot, slot51PlugSets);
/** Loaded item table: one index row per item definition. */
using ItemRow = generated::ItemIndexRow;
// Tag layout: the item table's array descriptor.
constexpr std::size_t kItemArrayDescriptorOffset = offsetof(generated::ItemIndexTable, table);

/** Installed reusable-set layout and the two rows proved by package extraction. */
constexpr std::size_t kTableArrayDescriptorOffset = offsetof(generated::C80802cb6, table);
constexpr std::size_t kPlugSetRowStride = sizeof(generated::SPlugSetRow);
constexpr std::size_t kPlugSetMemberDescriptorOffset = offsetof(generated::SPlugSetRow, entries);
constexpr std::size_t kPlugMemberStride = sizeof(generated::PlugSetMember);
constexpr std::size_t kGeneralSetIndex = 8;
constexpr std::size_t kLegSetIndex = 14;
constexpr std::uint64_t kGeneralMemberCount = 19;
constexpr std::uint64_t kLegMemberCount = 52;
constexpr std::size_t kMaximumSetCount = 4096;
constexpr std::size_t kMaximumMemberCount = 4096;
/** A definition reaches its plug block through this self-relative pointer. */
constexpr std::size_t kPlugBlockPointer = offsetof(generated::ItemDefinition, plugItemBlock);
/** The pointer names the block's fields; its class word sits just before them. */
constexpr std::size_t kPlugBlockClassSize =
    middleware::content::packages::reader::layout::kClassWordSize;
constexpr std::size_t kPlugCategoryOffset =
    kPlugBlockClassSize + offsetof(generated::PlugBlock, plugCategory);
/** Bytes compared: the class word up to the low word of the enabled rules' relative field. */
constexpr std::size_t kPlugBlockSize =
    kPlugBlockClassSize + offsetof(generated::PlugBlock, enabledRequirements.enabledRules.relative)
    + sizeof(std::uint32_t);
/** Plug-block bytes after the class and category, which must match the reference item exactly. */
constexpr std::size_t kPlugBlockMetadataOffset =
    kPlugCategoryOffset + sizeof(generated::PlugBlock::plugCategory);
constexpr std::size_t kPlugBlockMetadataSize = kPlugBlockSize - kPlugBlockMetadataOffset;

using native_array::add_relative;
using native_array::ArrayDescriptor;
using native_array::ArrayView;
using native_array::read;
using native_array::read_bytes;
using native_array::resolve_array;
using native_array::same;
using native_array::write;

struct LocatedSets {
    content_investment::Source source{};
    ArrayView general{};
    ArrayView legs{};
};

using Allocation = low_arena::Allocation;

struct AppliedSet {
    std::uintptr_t descriptor{};
    ArrayDescriptor original{};
    ArrayDescriptor replacement{};
};

struct CategoryPatch {
    std::uintptr_t address{};
    std::uint32_t original{};
};
/** Plug category the four mods ship with, and the one their leg-armour presentation needs. */
constexpr std::uint32_t kGeneralCategory = 0x94493B9BU;
constexpr std::uint32_t kLegCategory = socket_plugs::kArrivalsLegCategoryHash;
std::array<CategoryPatch, kArrivalsLegModHashes.size()> g_categories{};
std::size_t g_categoryCount{};

enum class Failure : std::uint8_t {
    none,
    buildData,
    targets,
    source,
    ambiguous,
    allocation,
    write,
    verification,
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Allocation, 2> g_allocations{};
std::array<AppliedSet, 2> g_applied{};
std::size_t g_appliedCount{};
Failure g_lastFailure{Failure::none};
std::array<std::array<relocation::Row, relocation::kMaximumMembers>, 2> g_expected{};
std::array<std::size_t, 2> g_expectedCounts{};
content_investment::Source g_categorySource{};
std::array<std::uint16_t, kArrivalsLegModHashes.size()> g_categoryRoutes{};
std::uint16_t g_categoryReference{};
const char* g_categoryFailure = "none";

/**
 * Maps one failure code to its log token.
 * @return "unknown" for a code outside the enum.
 */
[[nodiscard]] const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::buildData:
        return "build_data";
    case Failure::targets:
        return "targets";
    case Failure::source:
        return "source";
    case Failure::ambiguous:
        return "ambiguous";
    case Failure::allocation:
        return "allocation";
    case Failure::write:
        return "write";
    case Failure::verification:
        return "verification";
    case Failure::none:
        return "none";
    }
    return "unknown";
}

/**
 * Logs one deferral line, and only when the failure differs from the last one.
 * @param detail Failure-specific number carried in the line.
 */
void report_failure(Failure failure, std::uint64_t detail = 0) noexcept {
    if (failure == g_lastFailure) {
        return;
    }
    g_lastFailure = failure;
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=investment stage=arrivals_leg_sets result=deferred reason=%s detail=%llu",
                      failure_name(failure),
                      static_cast<unsigned long long>(detail));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::warn,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

[[nodiscard]] bool
member_index(const ArrayView& array, std::size_t position, std::uint32_t& index) noexcept {
    return position < array.count
           && read(array.data + static_cast<std::uintptr_t>(position) * kPlugMemberStride
                       + relocation::kMemberPlugItemOffset,
                   index);
}

/**
 * Counts how many rows of one plug set name the given item index.
 * @return A count over kMaximumMemberCount when a row could not be read.
 */
[[nodiscard]] std::size_t count_member(const ArrayView& array, std::uint32_t index) noexcept {
    std::size_t count = 0;
    for (std::size_t position = 0; position < array.count; ++position) {
        std::uint32_t current = 0;
        if (!member_index(array, position, current)) {
            return kMaximumMemberCount + 1U;
        }
        count += current == index ? 1U : 0U;
    }
    return count;
}

/** Validates the exact native pre-patch membership, not just its two row numbers. */
[[nodiscard]] bool native_membership(const ArrayView& general,
                                     const ArrayView& legs,
                                     std::span<const std::uint16_t> routes,
                                     std::uint16_t reference) noexcept {
    if (general.count != kGeneralMemberCount || legs.count != kLegMemberCount
        || general.elementClass != legs.elementClass || count_member(general, reference) != 0
        || count_member(legs, reference) != 1) {
        return false;
    }
    for (const std::uint16_t route : routes) {
        if (count_member(general, route) != 1 || count_member(legs, route) != 0) {
            return false;
        }
    }
    return true;
}

/** Resolves one investment candidate and accepts only the exact installed item/set relation. */
[[nodiscard]] bool resolve_candidate(const state::content::Definition& candidate,
                                     std::span<const std::uint16_t> routes,
                                     std::uint16_t reference,
                                     LocatedSets& output) noexcept {
    const auto& targets = targets::game::content::get();
    content_investment::Source source{};
    source.investmentGlobalsTag = candidate.tag;
    source.handles.tablesSlot = reinterpret_cast<std::uintptr_t>(targets.contentHandleTablesSlot);
    source.handles.read = &memory::read_current_process;

    std::uintptr_t globals = 0;
    std::uintptr_t root = 0;
    std::uintptr_t itemTable = 0;
    std::uintptr_t plugSetTable = 0;
    std::uint32_t rootTag = 0;
    std::uint32_t itemTableTag = 0;
    std::uint32_t plugSetTableTag = 0;
    ArrayView items{};
    if (!content_handles::resolve(source.handles, source.investmentGlobalsTag, globals)
        || !read(globals + kGlobalsRootTagOffset, rootTag)
        || !content_handles::resolve(source.handles, rootTag, root)
        || !read(root + kItemTableTagOffset, itemTableTag)
        || !content_handles::resolve(source.handles, itemTableTag, itemTable)
        || !resolve_array(itemTable + kItemArrayDescriptorOffset,
                          state::build_data::items::kDefinitionCapacity,
                          sizeof(ItemRow),
                          ItemRow::kClass,
                          items)
        || !read(root + kPlugSetTableTagOffset, plugSetTableTag)
        || !content_handles::resolve(source.handles, plugSetTableTag, plugSetTable)) {
        return false;
    }

    const auto matches_item = [&](std::uint16_t index, std::uint32_t hash) noexcept {
        if (index >= items.count) {
            return false;
        }
        std::uint32_t itemHash = 0;
        return read(items.data + static_cast<std::uintptr_t>(index) * sizeof(ItemRow)
                        + offsetof(ItemRow, itemHash),
                    itemHash)
               && itemHash == hash;
    };
    if (!matches_item(reference, kArrivalsLegReferenceHash)) {
        return false;
    }
    for (std::size_t index = 0; index < routes.size(); ++index) {
        if (!matches_item(routes[index], kArrivalsLegModHashes[index])) {
            return false;
        }
    }

    ArrayView sets{};
    if (!resolve_array(plugSetTable + kTableArrayDescriptorOffset,
                       kMaximumSetCount,
                       kPlugSetRowStride,
                       generated::SPlugSetRow::kClass,
                       sets)
        || sets.count <= kLegSetIndex) {
        return false;
    }
    const std::uintptr_t generalDescriptor =
        sets.data + kGeneralSetIndex * kPlugSetRowStride + kPlugSetMemberDescriptorOffset;
    const std::uintptr_t legDescriptor =
        sets.data + kLegSetIndex * kPlugSetRowStride + kPlugSetMemberDescriptorOffset;
    ArrayView general{};
    ArrayView legs{};
    if (!resolve_array(generalDescriptor,
                       kMaximumMemberCount,
                       kPlugMemberStride,
                       relocation::kMemberClass,
                       general)
        || !resolve_array(
            legDescriptor, kMaximumMemberCount, kPlugMemberStride, relocation::kMemberClass, legs)
        || !native_membership(general, legs, routes, reference)) {
        return false;
    }
    output = {source, general, legs};
    return true;
}

/** Finds exactly one native pair among every registered investment-globals candidate. */
[[nodiscard]] bool locate_sets(std::span<const std::uint16_t> routes,
                               std::uint16_t reference,
                               LocatedSets& output,
                               Failure& failure,
                               std::uint64_t& detail) noexcept {
    output = {};
    failure = Failure::source;
    detail = 0;
    if (!targets::game::content::is_resolved()
        || targets::game::content::get().contentHandleTablesSlot == nullptr) {
        failure = Failure::targets;
        return false;
    }
    std::array<state::content::Definition, kBootstrapCandidateCapacity> candidates{};
    std::size_t candidateCount = 0;
    if (!state::content::lookup_hash(kInvestmentGlobalsNameHash, candidates, candidateCount)
        && candidateCount == 0) {
        return false;
    }
    std::size_t matches = 0;
    for (std::size_t index = 0; index < candidateCount; ++index) {
        LocatedSets candidateSets{};
        if (!resolve_candidate(candidates[index], routes, reference, candidateSets)) {
            continue;
        }
        if (matches != 0
            && (candidateSets.general.descriptor != output.general.descriptor
                || candidateSets.legs.descriptor != output.legs.descriptor)) {
            failure = Failure::ambiguous;
            detail = matches + 1U;
            return false;
        }
        output = candidateSets;
        ++matches;
    }
    detail = matches;
    return matches != 0;
}

void release(Allocation& allocation) noexcept {
    // Storage belongs to the process-lifetime low arena, not to an individual array.
    allocation = {};
}

/** Copies one native member row and the single condition record it names. */
[[nodiscard]] bool
snapshot_row(const ArrayView& source, std::size_t position, relocation::Row& output) noexcept {
    output = {};
    const auto address = source.data + position * kPlugMemberStride;
    if (position >= source.count || address % relocation::kRecordAlignment != 0
        || source.elementClass != relocation::kMemberClass || !read_bytes(address, output.bytes)) {
        return false;
    }
    const auto count = relocation::get<std::uint64_t>(output.bytes.data()
                                                      + relocation::kMemberConditionCountOffset);
    if (count != 0) {
        const auto referenceAt = address + relocation::kMemberConditionRelativeOffset;
        std::uintptr_t reference = 0;
        if (count != 1
            || !add_relative(referenceAt,
                             relocation::get<std::int64_t>(
                                 output.bytes.data() + relocation::kMemberConditionRelativeOffset),
                             reference)
            || reference < relocation::kConditionReferenceOffset
            || reference % relocation::kRecordAlignment != 0
            || !read_bytes(reference - relocation::kConditionReferenceOffset, output.condition)) {
            return false;
        }
    }
    return relocation::valid(output);
}

/** Validates the exact plug blocks observed in this build before changing four category fields. */
bool stage_categories(const content_investment::Source& source,
                      std::span<const std::uint16_t> routes,
                      std::uint16_t reference,
                      std::span<CategoryPatch> output) noexcept {
    std::uintptr_t globals = 0, root = 0, table = 0;
    g_categoryFailure = "table";
    std::uint32_t tag = 0;
    ArrayView items{};
    if (!content_handles::resolve(source.handles, source.investmentGlobalsTag, globals)
        || !read(globals + kGlobalsRootTagOffset, tag)
        || !content_handles::resolve(source.handles, tag, root)
        || !read(root + kItemTableTagOffset, tag)
        || !content_handles::resolve(source.handles, tag, table)
        || !resolve_array(table + kItemArrayDescriptorOffset,
                          state::build_data::items::kDefinitionCapacity,
                          sizeof(ItemRow),
                          ItemRow::kClass,
                          items)) {
        return false;
    }
    const auto block = [&](std::uint16_t index,
                           std::uint32_t hash,
                           std::uintptr_t& address,
                           std::array<std::byte, kPlugBlockSize>& bytes) noexcept {
        const std::uintptr_t row =
            items.data + static_cast<std::uintptr_t>(index) * sizeof(ItemRow);
        std::uint32_t itemHash = 0;
        std::uint32_t target = 0;
        g_categoryFailure = "definition";
        std::uintptr_t definition = 0;
        if (!read(row + offsetof(ItemRow, itemHash), itemHash) || itemHash != hash
            || !read(row + offsetof(ItemRow, itemDefinition), target)
            || !content_handles::resolve(source.handles, target, definition)) {
            return false;
        }
        g_categoryFailure = "block";
        const std::uintptr_t pointer = definition + kPlugBlockPointer;
        std::int64_t relative = 0;
        std::uintptr_t fields = 0;
        if (!read(pointer, relative) || relative == 0 || !add_relative(pointer, relative, fields)
            || fields < kPlugBlockClassSize) {
            return false;
        }
        const std::uintptr_t start = fields - kPlugBlockClassSize;
        address = start + kPlugCategoryOffset;
        return read_bytes(start, bytes)
               && relocation::get<std::uint32_t>(bytes.data())
                      == middleware::content::packages::tables::kPlugBlockClass;
    };
    std::array<std::byte, kPlugBlockSize> referenceBlock{};
    std::uintptr_t referenceAddress = 0;
    if (!block(reference, kArrivalsLegReferenceHash, referenceAddress, referenceBlock)) {
        return false;
    }
    g_categoryFailure = "reference_category";
    if (relocation::get<std::uint32_t>(referenceBlock.data() + kPlugCategoryOffset)
        != kLegCategory) {
        return false;
    }
    for (std::size_t i = 0; i < routes.size(); ++i) {
        std::array<std::byte, kPlugBlockSize> bytes{};
        if (!block(routes[i], kArrivalsLegModHashes[i], output[i].address, bytes)) {
            return false;
        }
        output[i].original = relocation::get<std::uint32_t>(bytes.data() + kPlugCategoryOffset);
        g_categoryFailure = "category_or_metadata";
        if ((output[i].original != kGeneralCategory && output[i].original != kLegCategory)
            || std::memcmp(bytes.data() + kPlugBlockMetadataOffset,
                           referenceBlock.data() + kPlugBlockMetadataOffset,
                           kPlugBlockMetadataSize)
                   != 0) {
            return false;
        }
    }
    return true;
}

/** @return True while all four category fields hold the leg-armour value. */
bool categories_current() noexcept {
    if (g_categoryCount != g_categories.size()) {
        return false;
    }
    for (const auto& category : g_categories) {
        std::uint32_t value = 0;
        if (!read(category.address, value) || value != kLegCategory) {
            return false;
        }
    }
    return true;
}

/** Restore only fields still owned by this patch; retain ownership if any restore fails. */
bool restore_categories() noexcept {
    bool restored = true;
    for (std::size_t i = 0; i < g_categoryCount; ++i) {
        const auto& category = g_categories[i];
        std::uint32_t current = 0;
        if (!read(category.address, current)
            || (current != category.original
                && (current != kLegCategory || !write(category.address, category.original)))) {
            restored = false;
        }
    }
    if (restored) {
        g_categoryCount = 0;
    }
    return restored;
}

/** Rewrites the four plug categories once. A later relocation event gets its own attempt. */
void apply_categories() noexcept {
    if (g_categoryCount != 0 || g_categorySource.investmentGlobalsTag == 0) {
        return;
    }
    std::array<CategoryPatch, kArrivalsLegModHashes.size()> categories{};
    std::array<char, core::log::kLineCapacity> line{};
    if (!stage_categories(g_categorySource, g_categoryRoutes, g_categoryReference, categories)) {
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=investment stage=arrivals_leg_categories result=failed reason=%s",
                          g_categoryFailure);
        if (written > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::warn, line.data());
        }
        return;
    }
    g_categories = categories;
    g_categoryCount = categories.size();
    bool written = true;
    for (const auto& category : categories) {
        if (!write(category.address, kLegCategory)) {
            written = false;
            break;
        }
    }
    if (written && categories_current()) {
        const int length =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=investment stage=arrivals_leg_categories result=applied applied=%zu",
                          g_categoryCount);
        if (length > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info, line.data());
        }
        return;
    }
    (void)restore_categories();
    core::log::write(
        core::log::Channel::client,
        core::log::Level::warn,
        "ev=investment stage=arrivals_leg_categories result=failed reason=write_or_verify");
}

/**
 * Builds the count and self-relative displacement that point one descriptor at our array.
 * @param descriptor Address of the native descriptor the displacement is measured from.
 * @param count Member count to store in the descriptor.
 * @param output Receives the encoded descriptor.
 * @return False when the displacement does not fit a signed 64-bit value.
 */
[[nodiscard]] bool encode_descriptor(std::uintptr_t descriptor,
                                     const Allocation& allocation,
                                     std::uint64_t count,
                                     ArrayDescriptor& output) noexcept {
    const std::uintptr_t header =
        reinterpret_cast<std::uintptr_t>(allocation.base) + relocation::kArrayHeaderOffset;
    const std::uintptr_t relativeBase =
        descriptor + middleware::content::packages::reader::layout::kArrayFieldRelative;
    if (header >= relativeBase) {
        const std::uintptr_t distance = header - relativeBase;
        if (distance > static_cast<std::uintptr_t>((std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }
        output = {static_cast<std::int64_t>(count), static_cast<std::int64_t>(distance)};
        return true;
    }
    const std::uintptr_t distance = relativeBase - header;
    if (distance > static_cast<std::uintptr_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    output = {static_cast<std::int64_t>(count), -static_cast<std::int64_t>(distance)};
    return true;
}

/** Builds the general array with the four Arrivals leg mods removed. */
[[nodiscard]] bool build_general(const ArrayView& source,
                                 std::span<const std::uint16_t> routes,
                                 Allocation& allocation,
                                 ArrayDescriptor& descriptor) noexcept {
    const std::uint64_t newCount = source.count - routes.size();
    if (newCount != kGeneralMemberCount - kArrivalsLegModHashes.size()) {
        return false;
    }
    std::size_t written = 0;
    for (std::size_t position = 0; position < source.count; ++position) {
        std::uint32_t index = 0;
        if (!member_index(source, position, index)) {
            release(allocation);
            return false;
        }
        if (std::find(routes.begin(), routes.end(), index) != routes.end()) {
            continue;
        }
        if (written >= newCount || !snapshot_row(source, position, g_expected[0][written])) {
            return false;
        }
        ++written;
    }
    if (written != newCount) {
        release(allocation);
        return false;
    }
    g_expectedCounts[0] = written;
    allocation = low_arena::allocate(relocation::capacity(written));
    return allocation
           && relocation::build(std::span(g_expected[0].data(), written),
                                std::span(allocation.base, allocation.size))
           && encode_descriptor(source.descriptor, allocation, newCount, descriptor);
}

/** Moves each target's OWN original row and condition data into the leg array. */
[[nodiscard]] bool build_legs(const ArrayView& source,
                              const ArrayView& general,
                              std::span<const std::uint16_t> routes,
                              std::uint16_t reference,
                              Allocation& allocation,
                              ArrayDescriptor& descriptor) noexcept {
    const std::uint64_t newCount = source.count + routes.size();
    if (newCount != kLegMemberCount + kArrivalsLegModHashes.size()) {
        return false;
    }
    std::size_t referencePosition = source.count;
    for (std::size_t position = 0; position < source.count; ++position) {
        if (!snapshot_row(source, position, g_expected[1][position])) {
            release(allocation);
            return false;
        }
        std::uint32_t index = 0;
        if (!member_index(source, position, index)) {
            release(allocation);
            return false;
        }
        if (index == reference) {
            referencePosition = position;
        }
    }
    if (referencePosition == source.count) {
        release(allocation);
        return false;
    }
    for (std::size_t route = 0; route < routes.size(); ++route) {
        bool found = false;
        for (std::size_t position = 0; position < general.count; ++position) {
            std::uint32_t index = 0;
            if (!member_index(general, position, index)) {
                return false;
            }
            if (index != routes[route]) {
                continue;
            }
            if (found || !snapshot_row(general, position, g_expected[1][source.count + route])) {
                return false;
            }
            found = true;
        }
        if (!found) {
            return false;
        }
    }
    g_expectedCounts[1] = static_cast<std::size_t>(newCount);
    // Keep the native Empty Mod Socket first, followed by the four artifact mods.
    std::rotate(g_expected[1].begin() + 1,
                g_expected[1].begin() + static_cast<std::ptrdiff_t>(source.count),
                g_expected[1].begin() + static_cast<std::ptrdiff_t>(newCount));
    allocation = low_arena::allocate(relocation::capacity(g_expectedCounts[1]));
    return allocation
           && relocation::build(std::span(g_expected[1].data(), g_expectedCounts[1]),
                                std::span(allocation.base, allocation.size))
           && encode_descriptor(source.descriptor, allocation, newCount, descriptor);
}

/**
 * Checks the exact post-patch membership: each routed mod left general and joined legs.
 * @param routes Item indexes moved from the general set to the leg set.
 * @param reference Item index that must stay in the leg set exactly once.
 * @return False when either count or any routed row is not where the patch put it.
 */
[[nodiscard]] bool patched_membership(const ArrayView& general,
                                      const ArrayView& legs,
                                      std::span<const std::uint16_t> routes,
                                      std::uint16_t reference) noexcept {
    if (general.count != kGeneralMemberCount - routes.size()
        || legs.count != kLegMemberCount + routes.size() || count_member(legs, reference) != 1) {
        return false;
    }
    for (const std::uint16_t route : routes) {
        if (count_member(general, route) != 0 || count_member(legs, route) != 1) {
            return false;
        }
    }
    return true;
}

/** Fast steady-state check used by the callback fallback after synchronous application. */
[[nodiscard]] bool descriptors_current() noexcept {
    if (g_appliedCount != g_applied.size()) {
        return false;
    }
    ArrayDescriptor generalDescriptor{};
    ArrayDescriptor legDescriptor{};
    if (!read(g_applied[0].descriptor, generalDescriptor)
        || !read(g_applied[1].descriptor, legDescriptor)
        || !same(generalDescriptor, g_applied[0].replacement)
        || !same(legDescriptor, g_applied[1].replacement)) {
        return false;
    }
    return true;
}

/** Proves the native reader now walks the relocated pair, which no encoder here owns. */
[[nodiscard]] bool patched_sets_current(std::span<const std::uint16_t> routes,
                                        std::uint16_t reference) noexcept {
    if (!descriptors_current()) {
        return false;
    }
    ArrayView general{};
    ArrayView legs{};
    return resolve_array(g_applied[0].descriptor,
                         kMaximumMemberCount,
                         kPlugMemberStride,
                         relocation::kMemberClass,
                         general)
           && resolve_array(g_applied[1].descriptor,
                            kMaximumMemberCount,
                            kPlugMemberStride,
                            relocation::kMemberClass,
                            legs)
           && patched_membership(general, legs, routes, reference);
}

/** Restores descriptors only when they still name this module's allocations. */
bool restore_applied() noexcept {
    bool restoredAll = restore_categories();
    for (std::size_t index = g_appliedCount; index > 0; --index) {
        const AppliedSet& applied = g_applied[index - 1];
        ArrayDescriptor current{};
        const bool loaded = read(applied.descriptor, current);
        if (loaded && same(current, applied.original)) {
            continue;
        }
        // Write only over this module's own replacement, so a foreign descriptor is left alone.
        if (!loaded || !same(current, applied.replacement)
            || !write(applied.descriptor, applied.original)) {
            restoredAll = false;
        }
    }
    if (restoredAll) {
        for (Allocation& allocation : g_allocations) {
            release(allocation);
        }
        g_applied = {};
        g_appliedCount = 0;
        g_categoryCount = 0;
    }
    return restoredAll;
}

} // namespace

void reserve_socket_menu_routing_storage() noexcept {
    low_arena::reserve();
}

/** Moves exactly four members between two validated native reusable plug sets. */
void apply_socket_menu_routing() noexcept {
    if (!core::settings::get().client.socketMenuRouting) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    std::array<std::uint16_t, kArrivalsLegModHashes.size()> routeIndices{};
    std::uint16_t referenceIndex = UINT16_MAX;
    state::build_data::items::Definition definition{};
    bool mapped =
        state::build_data::find_item_definition_hash(kArrivalsLegReferenceHash, definition);
    if (mapped) {
        referenceIndex = definition.definitionIndex;
    }
    for (std::size_t route = 0; mapped && route < kArrivalsLegModHashes.size(); ++route) {
        mapped =
            state::build_data::find_item_definition_hash(kArrivalsLegModHashes[route], definition);
        if (mapped) {
            routeIndices[route] = definition.definitionIndex;
        }
    }
    if (!mapped) {
        report_failure(Failure::buildData);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (descriptors_current()) {
        apply_categories();
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (g_appliedCount != 0 || g_categoryCount != 0) {
        if (!restore_applied()) {
            report_failure(Failure::write);
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    // Never reuse published storage: native readers may still retain a pointer after restoration.

    LocatedSets located{};
    Failure failure = Failure::none;
    std::uint64_t detail = 0;
    if (!locate_sets(routeIndices, referenceIndex, located, failure, detail)) {
        report_failure(failure, detail);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    std::array<Allocation, 2> allocations{};
    std::array<ArrayDescriptor, 2> replacements{};
    if (!build_general(located.general, routeIndices, allocations[0], replacements[0])
        || !build_legs(located.legs,
                       located.general,
                       routeIndices,
                       referenceIndex,
                       allocations[1],
                       replacements[1])) {
        release(allocations[0]);
        release(allocations[1]);
        report_failure(Failure::allocation);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    ArrayDescriptor originalGeneral{};
    ArrayDescriptor originalLegs{};
    const bool originalsRead = read(located.general.descriptor, originalGeneral)
                               && read(located.legs.descriptor, originalLegs);
    if (!originalsRead) {
        report_failure(Failure::write);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // Record ownership BEFORE either write; a protection-restoration failure may follow a copy.
    g_allocations = allocations;
    g_applied = {{{located.general.descriptor, originalGeneral, replacements[0]},
                  {located.legs.descriptor, originalLegs, replacements[1]}}};
    g_appliedCount = g_applied.size();
    const bool generalWritten = write(located.general.descriptor, replacements[0]);
    const bool legsWritten = generalWritten && write(located.legs.descriptor, replacements[1]);
    if (!legsWritten) {
        restore_applied();
        report_failure(Failure::write);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    if (!patched_sets_current(routeIndices, referenceIndex)) {
        restore_applied();
        report_failure(Failure::verification);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    g_lastFailure = Failure::none;
    g_categorySource = located.source;
    g_categoryRoutes = routeIndices;
    g_categoryReference = referenceIndex;
    apply_categories();
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=investment stage=arrivals_leg_sets result=applied source_count=%llu "
        "leg_count=%llu source=0x%llX legs=0x%llX general_data=0x%llX leg_data=0x%llX "
        "rows=%zu order=empty_then_artifact",
        static_cast<unsigned long long>(kGeneralMemberCount - routeIndices.size()),
        static_cast<unsigned long long>(kLegMemberCount + routeIndices.size()),
        static_cast<unsigned long long>(located.general.descriptor),
        static_cast<unsigned long long>(located.legs.descriptor),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_allocations[0].base)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_allocations[1].base)),
        g_expectedCounts[0] + g_expectedCounts[1]);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void restore_socket_menu_routing() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    restore_applied();
    g_categorySource = {};
    g_lastFailure = Failure::none;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::client::hooks::network::investment
