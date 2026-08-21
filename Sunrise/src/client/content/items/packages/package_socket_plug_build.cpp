#include "package_socket_plug_build.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/runtime.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Sundial/native category families whose socket seed expands to every plug in that family. */
constexpr std::array<std::uint32_t, 3> kExpandableCategories{
    0xB134761EU,
    0x87727F34U,
    0x6C863692U,
};
/** Tracker sockets synthesize these three safe plug choices by socket type. */
constexpr std::array<std::uint32_t, 3> kTrackerPlugHashes{
    2'285'418'970U,
    2'302'094'943U,
    38'912'240U,
};
/** Native ordinary socket type whose choices are the synthetic tracker set. */
constexpr std::uint16_t kTrackerSocketType = 518;
/** FNV-1a constants make pool fingerprints stable and cheap. */
constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
/** One acquired-state rule starts with kind 1 and then its item-definition index. */
constexpr std::uint32_t kAcquisitionRuleKind = 1;
constexpr std::size_t kAcquisitionRuleSize = 8;

/** Reads one trivially copied scalar from a bounded package blob. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/** Visitor adapter that appends one list member to a bounded lane candidate. */
struct VisitorContext {
    SocketPlugBuild* build{};
    std::size_t itemDefinitionCount{};
};

/**
 * @param opaque Visitor context supplied by the socket build.
 * @param itemDefinitionIndex Package-provided plug index.
 * @return True when the plug was accepted into bounded scratch.
 */
[[nodiscard]] bool visit_member(void* opaque, std::uint32_t itemDefinitionIndex) noexcept {
    auto& context = *static_cast<VisitorContext*>(opaque);
    return context.build != nullptr
           && context.build->add(itemDefinitionIndex, context.itemDefinitionCount);
}

} // namespace

bool read_catalyst_acquisition_gates(const reader::Source& source,
                                     reader::Scratch& scratch,
                                     std::span<const std::byte> root,
                                     std::vector<std::byte>& blob,
                                     std::vector<catalysts::AcquisitionGate>& output) noexcept {
    output.clear();
    std::uint32_t tableTag = 0;
    tables::Array table{};
    if (!tables::slot_tag(root, tables::kSocketTypeTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, scratch, tableTag, blob)
        || !tables::find_array_at(blob, tables::kTableArrayDescriptor, table)
        || table.elementClass != tables::kSocketTypeTableClass || table.count == 0
        || table.count > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const std::uint64_t tableSize = table.count * tables::kSocketTypeRowStride;
    if (tableSize > blob.size() || table.dataOffset > blob.size() - tableSize) {
        return false;
    }

    output.resize(static_cast<std::size_t>(table.count));
    const std::span<const std::byte> bytes{blob};
    for (std::size_t index = 0; index < output.size(); ++index) {
        catalysts::AcquisitionGate& gate = output[index];
        gate.socketType = static_cast<std::uint16_t>(index);
        const std::size_t row = table.dataOffset + index * tables::kSocketTypeRowStride;
        tables::Array rules{};
        if (!tables::find_array_at(bytes, row + tables::kSocketTypeAcquisitionDescriptor, rules)) {
            continue;
        }
        if (rules.count != 1 || rules.elementClass != tables::kInvestmentExpressionRowClass
            || rules.dataOffset > bytes.size()
            || bytes.size() - rules.dataOffset < kAcquisitionRuleSize) {
            gate.state = catalysts::AcquisitionState::ambiguous;
            continue;
        }
        std::uint32_t kind = 0;
        std::uint32_t definitionIndex = 0;
        if (!read(bytes, rules.dataOffset, kind)
            || !read(bytes, rules.dataOffset + sizeof kind, definitionIndex)
            || kind != kAcquisitionRuleKind
            || definitionIndex >= state::build_data::items::kDefinitionCapacity) {
            gate.state = catalysts::AcquisitionState::ambiguous;
            continue;
        }
        gate.definitionIndex = static_cast<std::uint16_t>(definitionIndex);
        gate.state = catalysts::AcquisitionState::present;
    }
    return true;
}

bool read_catalyst_objective_values(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    std::span<const std::byte> root,
                                    std::vector<std::byte>& blob,
                                    std::vector<std::int32_t>& output) noexcept {
    output.clear();
    std::uint32_t tableTag = 0;
    std::uint32_t tableClass = 0;
    tables::Array table{};
    if (!tables::slot_tag(root, tables::kObjectiveTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, scratch, tableTag, blob, tableClass)
        || tableClass != tables::kObjectiveTableClass
        || !tables::find_array_at(blob, tables::kTableArrayDescriptor, table)
        || table.elementClass != tables::kObjectiveRowClass || table.count == 0
        || table.count > catalysts::kUnavailableObjectiveIndex) {
        return false;
    }
    const std::uint64_t tableSize = table.count * tables::kObjectiveRowStride;
    if (tableSize > blob.size() || table.dataOffset > blob.size() - tableSize) {
        return false;
    }

    output.resize(static_cast<std::size_t>(table.count));
    const std::span<const std::byte> bytes{blob};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const std::size_t offset = table.dataOffset + index * tables::kObjectiveRowStride
                                   + tables::kObjectiveCompletionValueOffset;
        if (!read(bytes, offset, output[index])) {
            output.clear();
            return false;
        }
    }
    return true;
}

/** Returns the compact 1-based code of one native category-expansion family. */
std::uint8_t special_plug_category(std::uint32_t categoryHash) noexcept {
    for (std::size_t index = 0; index < kExpandableCategories.size(); ++index) {
        if (kExpandableCategories[index] == categoryHash) {
            return static_cast<std::uint8_t>(index + 1);
        }
    }
    return 0;
}

/** Allocates the bounded build state and indexes expansion/tracker plug definitions. */
bool SocketPlugBuild::prepare(
    std::span<const std::uint8_t> specialCategories,
    std::span<const state::build_data::items::Definition> itemDefinitions) noexcept {
    release();
    if (specialCategories.size() < itemDefinitions.size()
        || itemDefinitions.size() > state::build_data::items::kDefinitionCapacity) {
        return false;
    }
    rules_.assign(socket_plugs::kRuleCapacity, {});
    pools_.assign(socket_plugs::kPoolCapacity, {});
    members_.assign(socket_plugs::kMemberCapacity, {});
    candidates_.assign(state::build_data::items::kDefinitionCapacity, {});
    categoryMembers_.assign(kCategoryCount * state::build_data::items::kDefinitionCapacity, {});
    lookup_.assign(kLookupCapacity, {});
    pools_[socket_plugs::kEmptyPoolIndex] = {};
    poolCount_ = 1;
    for (std::size_t item = 0; item < itemDefinitions.size(); ++item) {
        const std::uint8_t category = specialCategories[item];
        if (category != 0 && category <= kCategoryCount) {
            const std::size_t family = category - 1;
            categoryMembers_[family * state::build_data::items::kDefinitionCapacity
                             + categoryCounts_[family]++] = static_cast<std::uint16_t>(item);
        }
        for (const std::uint32_t trackerHash : kTrackerPlugHashes) {
            if (itemDefinitions[item].definitionHash != trackerHash) {
                continue;
            }
            if (trackerCount_ >= trackerMembers_.size()) {
                release();
                return false;
            }
            trackerMembers_[trackerCount_++] = static_cast<std::uint16_t>(item);
        }
    }
    return true;
}

/** Appends one package member after enforcing the installed item-table bound. */
bool SocketPlugBuild::add(std::uint32_t itemDefinitionIndex,
                          std::size_t itemDefinitionCount) noexcept {
    if (candidates_.empty() || itemDefinitionIndex >= itemDefinitionCount
        || itemDefinitionIndex >= state::build_data::items::kDefinitionCapacity
        || candidateCount_ >= state::build_data::items::kDefinitionCapacity) {
        return false;
    }
    candidates_[candidateCount_++] = static_cast<std::uint16_t>(itemDefinitionIndex);
    return true;
}

/** Expands special category seeds, sorts/deduplicates, then interns one exact pool. */
bool SocketPlugBuild::intern(std::uint32_t& poolIndex) noexcept {
    poolIndex = socket_plugs::kEmptyPoolIndex;
    if (candidates_.empty() || categoryMembers_.empty() || lookup_.empty()) {
        return false;
    }
    std::sort(candidates_.data(), candidates_.data() + candidateCount_);
    candidateCount_ = static_cast<std::size_t>(
        std::unique(candidates_.data(), candidates_.data() + candidateCount_) - candidates_.data());
    std::array<bool, kCategoryCount> expand{};
    // Category codes were indexed by native item definition index during prepare().
    for (std::size_t family = 0; family < kCategoryCount; ++family) {
        const auto* familyMembers =
            categoryMembers_.data() + family * state::build_data::items::kDefinitionCapacity;
        for (std::size_t seed = 0; seed < candidateCount_ && !expand[family]; ++seed) {
            expand[family] = std::binary_search(
                familyMembers, familyMembers + categoryCounts_[family], candidates_[seed]);
        }
    }
    for (std::size_t family = 0; family < kCategoryCount; ++family) {
        if (!expand[family]) {
            continue;
        }
        if (categoryCounts_[family]
            > state::build_data::items::kDefinitionCapacity - candidateCount_) {
            return false;
        }
        const auto* first =
            categoryMembers_.data() + family * state::build_data::items::kDefinitionCapacity;
        std::copy_n(first, categoryCounts_[family], candidates_.data() + candidateCount_);
        candidateCount_ += categoryCounts_[family];
    }
    std::sort(candidates_.data(), candidates_.data() + candidateCount_);
    candidateCount_ = static_cast<std::size_t>(
        std::unique(candidates_.data(), candidates_.data() + candidateCount_) - candidates_.data());
    if (candidateCount_ == 0) {
        return true;
    }

    std::uint64_t fingerprint = kHashOffsetBasis;
    for (std::size_t member = 0; member < candidateCount_; ++member) {
        std::uint16_t value = candidates_[member];
        for (std::size_t byte = 0; byte < sizeof value; ++byte) {
            fingerprint ^= static_cast<std::uint8_t>(value);
            fingerprint *= kHashPrime;
            value >>= 8U;
        }
    }
    fingerprint ^= candidateCount_;
    fingerprint *= kHashPrime;
    static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);
    const std::size_t start = static_cast<std::size_t>(fingerprint) & (kLookupCapacity - 1);
    for (std::size_t probe = 0; probe < kLookupCapacity; ++probe) {
        PoolLookup& slot = lookup_[(start + probe) & (kLookupCapacity - 1)];
        if (slot.poolIndex == UINT32_MAX) {
            if (poolCount_ >= socket_plugs::kPoolCapacity
                || candidateCount_ > socket_plugs::kMemberCapacity - memberCount_) {
                return false;
            }
            poolIndex = static_cast<std::uint32_t>(poolCount_);
            pools_[poolCount_++] = {static_cast<std::uint32_t>(memberCount_),
                                    static_cast<std::uint32_t>(candidateCount_)};
            std::copy_n(candidates_.data(), candidateCount_, members_.data() + memberCount_);
            memberCount_ += candidateCount_;
            slot = {fingerprint, poolIndex};
            return true;
        }
        if (slot.fingerprint != fingerprint || slot.poolIndex >= poolCount_) {
            continue;
        }
        const socket_plugs::Pool& pool = pools_[slot.poolIndex];
        if (pool.memberCount == candidateCount_
            && std::equal(candidates_.data(),
                          candidates_.data() + candidateCount_,
                          members_.data() + pool.memberOffset)) {
            poolIndex = slot.poolIndex;
            return true;
        }
    }
    return false;
}

/** Extracts every exact ordinary socket pool of one installed item definition. */
bool SocketPlugBuild::append(const tables::items::Row& item,
                             std::span<const std::byte> itemDefinition,
                             std::span<const std::byte> plugSetTable,
                             std::size_t itemDefinitionCount) noexcept {
    if (rules_.empty() || item.definitionIndex >= itemDefinitionCount
        || item.socketCount > socket_plugs::kLaneCapacity) {
        return false;
    }
    bool complete = true;
    for (std::uint8_t lane = 0; lane < item.socketCount; ++lane) {
        candidateCount_ = 0;
        VisitorContext visitor{this, itemDefinitionCount};
        bool laneValid = tables::items::visit_allowed_plugs(
            itemDefinition, plugSetTable, lane, visit_member, &visitor);
        if (laneValid && item.initialPlugs[lane] != tables::items::kUnavailablePlug) {
            laneValid = add(item.initialPlugs[lane], itemDefinitionCount);
        }
        if (laneValid && item.socketTypes[lane] == kTrackerSocketType) {
            for (std::size_t tracker = 0; tracker < trackerCount_ && laneValid; ++tracker) {
                laneValid = add(trackerMembers_[tracker], itemDefinitionCount);
            }
        }
        std::uint32_t poolIndex = socket_plugs::kEmptyPoolIndex;
        laneValid = laneValid && intern(poolIndex);
        if (!laneValid || ruleCount_ >= socket_plugs::kRuleCapacity) {
            ++skipped_;
            complete = false;
            continue;
        }
        rules_[ruleCount_++] = {item.definitionIndex, lane, 0, poolIndex};
    }
    return complete;
}

/** Publishes the bounded relation and retains its rows for dependent package builders. */
bool SocketPlugBuild::publish() noexcept {
    return !rules_.empty() && !pools_.empty() && !members_.empty()
           && state::build_data::publish_socket_plug_rules(
               std::span(rules_.data(), ruleCount_),
               std::span(pools_.data(), poolCount_),
               std::span(members_.data(), memberCount_));
}

/** Reports how many lanes failed closed during extraction. */
std::size_t SocketPlugBuild::skipped() const noexcept {
    return skipped_;
}

std::size_t SocketPlugBuild::rule_count() const noexcept {
    return ruleCount_;
}

std::size_t SocketPlugBuild::pool_count() const noexcept {
    return poolCount_;
}

std::size_t SocketPlugBuild::member_count() const noexcept {
    return memberCount_;
}

std::span<const socket_plugs::Rule> SocketPlugBuild::rules() const noexcept {
    return std::span(rules_).first(ruleCount_);
}

std::span<const socket_plugs::Pool> SocketPlugBuild::pools() const noexcept {
    return std::span(pools_).first(poolCount_);
}

std::span<const socket_plugs::Member> SocketPlugBuild::members() const noexcept {
    return std::span(members_).first(memberCount_);
}

/** Drops all heap-backed extraction scratch and resets every count. */
void SocketPlugBuild::release() noexcept {
    rules_.clear();
    rules_.shrink_to_fit();
    pools_.clear();
    pools_.shrink_to_fit();
    members_.clear();
    members_.shrink_to_fit();
    candidates_.clear();
    candidates_.shrink_to_fit();
    categoryMembers_.clear();
    categoryMembers_.shrink_to_fit();
    lookup_.clear();
    lookup_.shrink_to_fit();
    categoryCounts_ = {};
    trackerMembers_ = {};
    trackerCount_ = 0;
    ruleCount_ = 0;
    poolCount_ = 0;
    memberCount_ = 0;
    candidateCount_ = 0;
    skipped_ = 0;
}

} // namespace sunrise::client::content::items::packages
