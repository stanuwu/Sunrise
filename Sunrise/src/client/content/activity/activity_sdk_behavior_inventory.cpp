#include "activity_sdk_behavior_inventory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/parallel.h"
#include "../../../middleware/content/packages/reader/typed_array.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808029.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80802f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808031.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808039.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80803e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808044.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804d.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804e.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80804f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80805f.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808067.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808072.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808084.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808092.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808093.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808094.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809a.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "activity_sdk_behavior_node_shapes.h"

namespace sunrise::client::content::activity::sdk_generation::behavior_inventory {
namespace {

namespace reader = middleware::content::packages::reader;
namespace parallel = middleware::content::packages::reader::parallel;
namespace generated = middleware::content::packages::schema::generated;

// Package class ids of the behavior graph nodes this pass reads.
constexpr std::uint32_t kRootClass = generated::ObjectBehaviorRoot::kClass;
constexpr std::uint32_t kStorageChannelClass = generated::BehaviorStorageChannel::kClass;
constexpr std::uint32_t kConditionClass = generated::BehaviorChannelComparison::kClass;
constexpr std::uint32_t kFilterClass = generated::BehaviorFilter::kClass;
constexpr std::uint32_t kTargetClass = generated::BehaviorTarget::kClass;
constexpr std::uint32_t kConditionGroupClass = generated::BehaviorConditionGroup::kClass;
// Extraction bounds; one malformed blob must not exhaust memory.
constexpr std::uint32_t kMaximumRows = 1U << 20U;
constexpr std::uint32_t kMaximumDepth = 256U;
// Element classes an actor-definition array may carry.
constexpr std::uint32_t kBuildElementClass = generated::SPatternComponent::kClass;
constexpr std::uint32_t kDescriptorElementClass = generated::ActorDescriptorRow::kClass;
constexpr std::uint32_t kMetadataElementClass = generated::ActorDescriptorMetadata::kClass;
// A class id word sits just before each inline struct; array rows follow the typed-array header.
constexpr std::size_t kClassWordSize = reader::layout::kClassWordSize;
constexpr std::size_t kHeaderSize = reader::layout::TypedArrayHeader::kSize;
// A behavior root lists graph elements; each element points at one node.
constexpr std::size_t kRootCountOffset = offsetof(generated::ObjectBehaviorRoot, behaviors.count);
constexpr std::size_t kRootRelativeOffset =
    offsetof(generated::ObjectBehaviorRoot, behaviors.relative);
constexpr std::size_t kElementStride = sizeof(generated::SBehaviorGraphElement);
constexpr std::size_t kElementNodeOffset = offsetof(generated::SBehaviorGraphElement, behavior);
using Expression = generated::C80809419;
// A compiled expression lists its channel inputs, then carries its evaluator words.
constexpr std::size_t kInputCountOffset = offsetof(Expression, inputs.count);
constexpr std::size_t kInputRelativeOffset = offsetof(Expression, inputs.relative);
constexpr std::size_t kInputStride = sizeof(generated::BehaviorExpressionInput);
constexpr std::size_t kInputSelectorOffset =
    offsetof(generated::BehaviorExpressionInput, objectSelector);
constexpr std::size_t kInputChannelOffset = offsetof(generated::BehaviorExpressionInput, channel);
constexpr std::size_t kStorageChannelHashOffset =
    offsetof(generated::BehaviorStorageChannel, channelHash);
constexpr std::size_t kInputOrModeOffset = offsetof(Expression, expression.unknown56One);
constexpr std::size_t kActiveFieldOffset = offsetof(Expression, expression.activeField);
constexpr std::size_t kNativeOverrideOffset = offsetof(Expression, expression.evaluatorOverride);
// Filters and condition groups list filter rows; each row points at one node.
constexpr std::size_t kFilterRowStride = sizeof(generated::C80809316);
constexpr std::size_t kFilterRowNodeOffset = offsetof(generated::C80809316, filter);
constexpr std::size_t kFilterCountOffset =
    offsetof(generated::BehaviorFilter, filters.filtersInline.filters.count);
constexpr std::size_t kFilterRelativeOffset =
    offsetof(generated::BehaviorFilter, filters.filtersInline.filters.relative);
constexpr std::size_t kGroupCountOffset =
    offsetof(generated::BehaviorConditionGroup, filters.filters.count);
constexpr std::size_t kGroupRelativeOffset =
    offsetof(generated::BehaviorConditionGroup, filters.filters.relative);
// A comparison holds two expressions; a target points at its action node.
constexpr std::size_t kConditionLeftOffset =
    offsetof(generated::BehaviorChannelComparison, h70b1574d);
constexpr std::size_t kConditionRightOffset =
    offsetof(generated::BehaviorChannelComparison, h8c8ef163);
constexpr std::size_t kTargetActionOffset = offsetof(generated::BehaviorTarget, action);
// An actor definition lists its builds, interface descriptors and descriptor metadata.
constexpr std::size_t kBuildCountOffset = offsetof(generated::SEntity, components.count);
constexpr std::size_t kBuildRelativeOffset = offsetof(generated::SEntity, components.relative);
constexpr std::size_t kBuildStride = sizeof(generated::SPatternComponent);
constexpr std::size_t kBuildConfigOffset = offsetof(generated::SPatternComponent, config);
constexpr std::size_t kDescriptorCountOffset =
    offsetof(generated::SEntity, h83bfb495.interfaceDescriptors.count);
constexpr std::size_t kDescriptorRelativeOffset =
    offsetof(generated::SEntity, h83bfb495.interfaceDescriptors.relative);
constexpr std::size_t kDescriptorStride = sizeof(generated::ActorDescriptorRow);
constexpr std::size_t kDescriptorSubtypeOffset =
    offsetof(generated::ActorDescriptorRow, componentKey);
constexpr std::size_t kDescriptorConfigOffset = offsetof(generated::ActorDescriptorRow, config.tag);
constexpr std::size_t kMetadataCountOffset =
    offsetof(generated::SEntity, h83bfb495.linksD24ByteRowsConfigAt0.count);
constexpr std::size_t kMetadataRelativeOffset =
    offsetof(generated::SEntity, h83bfb495.linksD24ByteRowsConfigAt0.relative);
constexpr std::size_t kMetadataStride = sizeof(generated::ActorDescriptorMetadata);
constexpr std::size_t kMetadataConfigOffset =
    offsetof(generated::ActorDescriptorMetadata, config.tag);
constexpr std::size_t kMetadataSubtypeOffset =
    offsetof(generated::ActorDescriptorMetadata, config.word);
constexpr std::size_t kMetadataBuildOffset =
    offsetof(generated::ActorDescriptorMetadata, buildOrdinal);
// All-ones marks an absent row index in every extracted table.
constexpr std::uint32_t kAbsentIndex = (std::numeric_limits<std::uint32_t>::max)();

enum class InputRole : std::uint8_t {
    action = 1,
    conditionLeft = 2,
    conditionRight = 3,
};

using node_shapes::NodeShape;

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> bytes, std::size_t offset, Value& output) noexcept {
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&output, bytes.data() + offset, sizeof(Value));
    return true;
}

/** Resolves one signed self-relative field only when its target stays inside the blob. */
[[nodiscard]] bool
relative_target(std::span<const std::byte> bytes, std::size_t field, std::size_t& output) noexcept {
    std::int64_t relative = 0;
    if (!read_value(bytes, field, relative) || relative == 0) {
        return false;
    }
    if ((relative > 0
         && static_cast<std::uint64_t>(relative) > bytes.size() - (std::min)(field, bytes.size()))
        || (relative < 0 && static_cast<std::uint64_t>(-(relative + 1)) + 1U > field)) {
        return false;
    }
    output = static_cast<std::size_t>(static_cast<std::int64_t>(field) + relative);
    return output <= bytes.size();
}

/** Walks one behavior program's bytes, collecting its inputs and channel writes. */
class Parser final {
public:
    Parser(std::span<const std::byte> bytes,
           std::uint32_t programRow,
           std::vector<Input>& inputs,
           std::vector<ChannelWrite>& writes) noexcept
        : bytes_(bytes), programRow_(programRow), inputs_(inputs), writes_(writes) {}

    /** Walks one root and retains the channel edges exposed by known node layouts. */
    [[nodiscard]] bool parse(Program& output) {
        output.firstInput = static_cast<std::uint32_t>(inputs_.size());
        output.firstWrite = static_cast<std::uint32_t>(writes_.size());
        if (!root(0, 0)) {
            return false;
        }
        output.inputCount = static_cast<std::uint32_t>(inputs_.size()) - output.firstInput;
        output.writeCount = static_cast<std::uint32_t>(writes_.size()) - output.firstWrite;
        output.nodeCount = static_cast<std::uint32_t>(nodes_.size());
        output.expressionCount = static_cast<std::uint32_t>(expressions_.size());
        return true;
    }

private:
    /** Reads one bounded resource array with its repeated count. */
    [[nodiscard]] bool array(std::size_t countField,
                             std::size_t relativeField,
                             std::size_t stride,
                             std::uint64_t& count,
                             std::size_t& data) const noexcept {
        std::size_t header = 0;
        std::uint64_t repeated = 0;
        if (!read_value(bytes_, countField, count) || count > kMaximumRows) {
            return false;
        }
        if (count == 0) {
            data = 0;
            return true;
        }
        if (!relative_target(bytes_, relativeField, header) || !read_value(bytes_, header, repeated)
            || repeated != count || bytes_.size() - header < kHeaderSize
            || count > (bytes_.size() - header - kHeaderSize) / stride) {
            return false;
        }
        data = header + kHeaderSize;
        return true;
    }

    /** Visits one behavior-node array once within the depth bound. */
    [[nodiscard]] bool root(std::size_t offset, std::uint32_t depth) {
        if (depth > kMaximumDepth || offset > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        if (!roots_.insert(static_cast<std::uint32_t>(offset)).second) {
            return true;
        }
        std::uint64_t count = 0;
        std::size_t data = 0;
        if (!array(offset + kRootCountOffset,
                   offset + kRootRelativeOffset,
                   kElementStride,
                   count,
                   data)) {
            return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::size_t field =
                data + static_cast<std::size_t>(index) * kElementStride + kElementNodeOffset;
            std::size_t child = 0;
            if (!relative_target(bytes_, field, child) || !node(child, depth + 1U)) {
                return false;
            }
        }
        return true;
    }

    /** Retains every named channel input from one compiled expression. */
    [[nodiscard]] bool expression(std::size_t offset, std::uint32_t nodeOffset, InputRole role) {
        if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        const auto [_, inserted] = expressions_.insert(static_cast<std::uint32_t>(offset));
        if (!inserted) {
            return true;
        }
        std::uint64_t count = 0;
        std::size_t data = 0;
        std::uint64_t inputOrMode = 0;
        std::uint32_t activeField = 0;
        std::int32_t nativeOverride = 0;
        if (!array(offset + kInputCountOffset,
                   offset + kInputRelativeOffset,
                   kInputStride,
                   count,
                   data)
            || !read_value(bytes_, offset + kInputOrModeOffset, inputOrMode)
            || !read_value(bytes_, offset + kActiveFieldOffset, activeField)
            || !read_value(bytes_, offset + kNativeOverrideOffset, nativeOverride)) {
            return false;
        }
        if (inputs_.size() > kMaximumRows - count) {
            return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::size_t row = data + static_cast<std::size_t>(index) * kInputStride;
            std::uint8_t selector = 0;
            std::uint32_t hash = 0;
            if (!read_value(bytes_, row + kInputSelectorOffset, selector)
                || !read_value(bytes_, row + kInputChannelOffset, hash)) {
                return false;
            }
            inputs_.push_back({programRow_,
                               nodeOffset,
                               static_cast<std::uint32_t>(offset),
                               hash,
                               inputOrMode,
                               nativeOverride,
                               activeField,
                               selector,
                               static_cast<std::uint8_t>(role)});
        }
        return true;
    }

    /** Visits every polymorphic node referenced by one resource array. */
    [[nodiscard]] bool
    node_array(std::size_t countField, std::size_t relativeField, std::uint32_t depth) {
        std::uint64_t count = 0;
        std::size_t data = 0;
        if (!array(countField, relativeField, kFilterRowStride, count, data)) {
            return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::size_t field =
                data + static_cast<std::size_t>(index) * kFilterRowStride + kFilterRowNodeOffset;
            std::size_t child = 0;
            if (!relative_target(bytes_, field, child) || !node(child, depth + 1U)) {
                return false;
            }
        }
        return true;
    }

    /** Walks one known node layout and its nested roots. */
    [[nodiscard]] bool node(std::size_t offset, std::uint32_t depth) {
        if (depth > kMaximumDepth || offset < kClassWordSize
            || offset > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        if (!nodes_.insert(static_cast<std::uint32_t>(offset)).second) {
            return true;
        }
        std::uint32_t classId = 0;
        if (!read_value(bytes_, offset - kClassWordSize, classId)) {
            return false;
        }
        const NodeShape* const spec = node_shapes::shape(classId);
        if (spec == nullptr || offset > bytes_.size() || spec->size > bytes_.size() - offset) {
            return false;
        }
        if (spec->expressionOffset != 0
            && !expression(offset + spec->expressionOffset,
                           static_cast<std::uint32_t>(offset),
                           InputRole::action)) {
            return false;
        }
        if (classId == kStorageChannelClass) {
            std::uint32_t hash = 0;
            if (!read_value(bytes_, offset + kStorageChannelHashOffset, hash)
                || writes_.size() >= kMaximumRows) {
                return false;
            }
            writes_.push_back({programRow_, static_cast<std::uint32_t>(offset), hash});
        }
        if (classId == kConditionClass
            && (!expression(offset + kConditionLeftOffset,
                            static_cast<std::uint32_t>(offset),
                            InputRole::conditionLeft)
                || !expression(offset + kConditionRightOffset,
                               static_cast<std::uint32_t>(offset),
                               InputRole::conditionRight))) {
            return false;
        }
        if (classId == kFilterClass
            && !node_array(offset + kFilterCountOffset, offset + kFilterRelativeOffset, depth)) {
            return false;
        }
        if (classId == kConditionGroupClass
            && !node_array(offset + kGroupCountOffset, offset + kGroupRelativeOffset, depth)) {
            return false;
        }
        if (classId == kTargetClass) {
            std::size_t child = 0;
            std::int64_t relative = 0;
            if (!read_value(bytes_, offset + kTargetActionOffset, relative)) {
                return false;
            }
            if (relative != 0
                && (!relative_target(bytes_, offset + kTargetActionOffset, child)
                    || !node(child, depth + 1U))) {
                return false;
            }
        }
        for (std::uint8_t index = 0; index < spec->rootCount; ++index) {
            if (!root(offset + spec->roots[index], depth + 1U)) {
                return false;
            }
        }
        return true;
    }

    std::span<const std::byte> bytes_{};
    std::uint32_t programRow_{};
    std::vector<Input>& inputs_;
    std::vector<ChannelWrite>& writes_;
    std::unordered_set<std::uint32_t> roots_{};
    std::unordered_set<std::uint32_t> nodes_{};
    std::unordered_set<std::uint32_t> expressions_{};
};

struct ParsedProgram final {
    Program program{};
    std::vector<Input> inputs{};
    std::vector<ChannelWrite> writes{};
    bool graphParsed{};
};

struct BuildOwner final {
    std::uint32_t actorClassIndex{};
    std::uint32_t configTag{};
    std::uint32_t buildOrdinal{};
    std::uint32_t descriptorOrdinal{kAbsentIndex};
    std::uint32_t submitterSubtype{};
};

/** Maps one proved submitter subtype without guessing unresolved rows. */
[[nodiscard]] SubmissionKind submission_kind(std::uint32_t subtype) noexcept {
    switch (subtype) {
    case generated::C80804dba::kClass:
    case generated::C80804da6::kClass:
    case generated::C80804dd1::kClass:
    case generated::C80804dc2::kClass:
    case generated::C80804dac::kClass:
    case generated::C80806779::kClass:
    case generated::C80804dbe::kClass:
    case generated::C80804d8d::kClass:
    case generated::C80804dcc::kClass:
    case generated::C80804dc0::kClass:
    case generated::C80804deb::kClass:
    case generated::C80802f92::kClass:
    case generated::C80804fbb::kClass:
        return SubmissionKind::activeNative;
    case generated::C80804de4::kClass:
    case generated::C80804fb0::kClass:
    case generated::C80806785::kClass:
    case generated::C808031d1::kClass:
    case generated::C808072c0::kClass:
    case generated::UnknownWeaponPrimaryComponent::kClass:
    case generated::C80805fb3::kClass:
    case generated::C80805fb0::kClass:
    case generated::C80804db2::kClass:
    case generated::C80804491::kClass:
    case generated::C80803926::kClass:
    case generated::UnknownTriggerPhysicsShape::kClass:
    case generated::C80802ff8::kClass:
    case generated::C80806782::kClass:
    case generated::C808072cb::kClass:
    case generated::C808084d7::kClass:
        return SubmissionKind::passive;
    default:
        return SubmissionKind::unresolved;
    }
}

/** Reads one typed actor-definition array within the source blob. */
[[nodiscard]] bool array(std::span<const std::byte> bytes,
                         std::size_t countField,
                         std::size_t relativeField,
                         std::size_t stride,
                         std::uint32_t elementClass,
                         std::uint64_t& count,
                         std::size_t& data) noexcept {
    std::int64_t relative = 0;
    if (!read_value(bytes, countField, count) || !read_value(bytes, relativeField, relative)
        || count > kMaximumRows) {
        return false;
    }
    if (count == 0 && relative == 0) {
        data = 0;
        return true;
    }
    if (count == 0 || relative == 0) {
        return false;
    }
    std::size_t header = 0;
    reader::TypedArrayRows rows{};
    if (!relative_target(bytes, relativeField, header)
        || !reader::read_typed_array(bytes, header, count, elementClass, stride, rows)) {
        return false;
    }
    data = rows.data;
    return true;
}

/** Projects the behavior-owner candidates from one actor definition. */
[[nodiscard]] bool actor_builds(std::span<const std::byte> bytes,
                                std::uint32_t actorClassIndex,
                                std::vector<BuildOwner>& output,
                                std::vector<std::uint32_t>& configTags) {
    std::uint64_t buildCount = 0;
    std::int64_t buildRelative = 0;
    if (!read_value(bytes, kBuildCountOffset, buildCount)
        || !read_value(bytes, kBuildRelativeOffset, buildRelative) || buildCount > kMaximumRows) {
        return false;
    }
    if (buildCount == 0 && buildRelative == 0) {
        return true;
    }
    if (buildCount == 0 || buildRelative < 0) {
        return false;
    }
    // The typed-array header sits at the field plus its non-negative relative value.
    const std::size_t header = kBuildRelativeOffset + static_cast<std::size_t>(buildRelative);
    reader::TypedArrayRows builds{};
    if (!reader::read_typed_array(
            bytes, header, buildCount, kBuildElementClass, kBuildStride, builds)) {
        return false;
    }
    std::uint64_t descriptorCount = 0;
    std::uint64_t metadataCount = 0;
    std::size_t descriptorData = 0;
    std::size_t metadataData = 0;
    if (!array(bytes,
               kDescriptorCountOffset,
               kDescriptorRelativeOffset,
               kDescriptorStride,
               kDescriptorElementClass,
               descriptorCount,
               descriptorData)
        || !array(bytes,
                  kMetadataCountOffset,
                  kMetadataRelativeOffset,
                  kMetadataStride,
                  kMetadataElementClass,
                  metadataCount,
                  metadataData)) {
        return false;
    }
    const std::uint64_t pairedDescriptorCount = (std::min)(descriptorCount, metadataCount);
    const std::size_t buildData = builds.data;
    for (std::uint64_t build = 0; build < buildCount; ++build) {
        std::uint32_t configTag = 0;
        if (!read_value(bytes,
                        buildData + static_cast<std::size_t>(build) * kBuildStride
                            + kBuildConfigOffset,
                        configTag)) {
            return false;
        }
        if (configTag == 0 || configTag == kAbsentIndex) {
            continue;
        }
        configTags.push_back(configTag);
        bool exposed = false;
        for (std::uint64_t descriptor = 0; descriptor < pairedDescriptorCount; ++descriptor) {
            const std::size_t row =
                metadataData + static_cast<std::size_t>(descriptor) * kMetadataStride;
            const std::size_t descriptorRow =
                descriptorData + static_cast<std::size_t>(descriptor) * kDescriptorStride;
            std::uint32_t metadataConfig = 0;
            std::uint32_t subtype = 0;
            std::uint32_t metadataBuild = 0;
            std::uint32_t descriptorSubtype = 0;
            std::uint32_t descriptorConfig = 0;
            if (!read_value(bytes, row + kMetadataConfigOffset, metadataConfig)
                || !read_value(bytes, row + kMetadataSubtypeOffset, subtype)
                || !read_value(bytes, row + kMetadataBuildOffset, metadataBuild)
                || !read_value(bytes, descriptorRow + kDescriptorSubtypeOffset, descriptorSubtype)
                || !read_value(bytes, descriptorRow + kDescriptorConfigOffset, descriptorConfig)) {
                return false;
            }
            if (metadataConfig == configTag && metadataBuild == build
                && descriptorConfig == configTag && descriptorSubtype == subtype) {
                output.push_back({actorClassIndex,
                                  configTag,
                                  static_cast<std::uint32_t>(build),
                                  static_cast<std::uint32_t>(descriptor),
                                  subtype});
                exposed = true;
            }
        }
        if (!exposed) {
            output.push_back(
                {actorClassIndex, configTag, static_cast<std::uint32_t>(build), kAbsentIndex, 0});
        }
    }
    return true;
}

/** Joins readable actor configs to installed roots without requiring optional coverage. */
[[nodiscard]] bool build_owners(const reader::Source& source,
                                std::span<const std::uint32_t> actorTags,
                                CancelProbe cancel,
                                void* cancelContext,
                                Snapshot& output) {
    if (actorTags.empty()) {
        return true;
    }
    // Held to the end, since the kept blobs are read up to the owner join.
    const parallel::ScopedRead read{};
    std::vector<parallel::Held> actors{};
    if (!parallel::read_kept(source, actorTags, actors)) {
        return false;
    }
    output.unreadActors = static_cast<std::uint32_t>(actorTags.size() - actors.size());
    std::vector<BuildOwner> builds{};
    std::vector<std::uint32_t> configTags{};
    for (const parallel::Held& actor : actors) {
        if (cancel != nullptr && cancel(cancelContext)) {
            return false;
        }
        const auto actorTag = std::lower_bound(actorTags.begin(), actorTags.end(), actor.tag);
        if (actorTag == actorTags.end() || *actorTag != actor.tag) {
            ++output.skippedActors;
            continue;
        }
        std::vector<BuildOwner> actorBuilds{};
        std::vector<std::uint32_t> actorConfigs{};
        if (!actor_builds(actor.blob,
                          static_cast<std::uint32_t>(actorTag - actorTags.begin()),
                          actorBuilds,
                          actorConfigs)) {
            ++output.skippedActors;
            continue;
        }
        builds.insert(builds.end(), actorBuilds.begin(), actorBuilds.end());
        configTags.insert(configTags.end(), actorConfigs.begin(), actorConfigs.end());
    }
    std::sort(configTags.begin(), configTags.end());
    configTags.erase(std::unique(configTags.begin(), configTags.end()), configTags.end());
    std::vector<parallel::Held> configs{};
    if (!parallel::read_kept(source, configTags, configs)) {
        return false;
    }
    output.unreadConfigs = static_cast<std::uint32_t>(configTags.size() - configs.size());
    std::unordered_map<std::uint32_t, std::uint32_t> programs{};
    programs.reserve(output.programs.size());
    for (std::size_t index = 0; index < output.programs.size(); ++index) {
        programs.emplace(output.programs[index].rootTag, static_cast<std::uint32_t>(index));
    }
    std::unordered_multimap<std::uint32_t, const BuildOwner*> owners{};
    owners.reserve(builds.size());
    for (const BuildOwner& build : builds) {
        owners.emplace(build.configTag, &build);
    }
    for (const parallel::Held& config : configs) {
        const auto [first, last] = owners.equal_range(config.tag);
        for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= config.blob.size();
             offset += sizeof(std::uint32_t)) {
            std::uint32_t rootTag = 0;
            std::memcpy(&rootTag, config.blob.data() + offset, sizeof rootTag);
            const auto program = programs.find(rootTag);
            if (program == programs.end()) {
                continue;
            }
            for (auto owner = first; owner != last; ++owner) {
                const BuildOwner& build = *owner->second;
                output.owners.push_back({program->second,
                                         build.actorClassIndex,
                                         config.tag,
                                         static_cast<std::uint32_t>(offset),
                                         build.buildOrdinal,
                                         build.descriptorOrdinal,
                                         build.submitterSubtype,
                                         submission_kind(build.submitterSubtype)});
            }
        }
    }
    std::sort(
        output.owners.begin(), output.owners.end(), [](const Owner& first, const Owner& second) {
            return std::tie(first.programRow,
                            first.actorClassIndex,
                            first.configTag,
                            first.configFieldOffset,
                            first.buildOrdinal,
                            first.descriptorOrdinal)
                   < std::tie(second.programRow,
                              second.actorClassIndex,
                              second.configTag,
                              second.configFieldOffset,
                              second.buildOrdinal,
                              second.descriptorOrdinal);
        });
    return true;
}

struct BuildContext final {
    CancelProbe cancel{};
    void* cancelContext{};
    std::vector<std::vector<ParsedProgram>>* workers{};
    std::atomic<bool> failed{};
};

/** Collects the fixed class scan before any encrypted package reads begin. */
[[nodiscard]] bool collect_tag(void* opaque, std::uint32_t tag) noexcept {
    auto& tags = *static_cast<std::vector<std::uint32_t>*>(opaque);
    try {
        tags.push_back(tag);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool cancelled(const BuildContext& context) noexcept {
    return context.cancel != nullptr && context.cancel(context.cancelContext);
}

/** Reports row-local omissions without turning them into a publication gate. */
void log_summary(const Snapshot& snapshot) noexcept {
    std::array<char, 320> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "activity_sdk_behaviors programs=%zu parsed=%u unparsed=%u "
                      "unread=%u inputs=%zu writes=%zu owners=%zu actors_skipped=%u "
                      "actors_unread=%u configs_unread=%u",
                      snapshot.programs.size(),
                      snapshot.parsedPrograms,
                      snapshot.unparsedPrograms,
                      snapshot.unreadPrograms,
                      snapshot.inputs.size(),
                      snapshot.writes.size(),
                      snapshot.owners.size(),
                      snapshot.skippedActors,
                      snapshot.unreadActors,
                      snapshot.unreadConfigs);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Parses one readable root on the reader thread and keeps failures row-local. */
void collect(void* opaque,
             std::size_t worker,
             std::uint32_t tag,
             std::span<const std::byte> bytes) noexcept {
    auto& context = *static_cast<BuildContext*>(opaque);
    if (context.failed.load(std::memory_order_relaxed) || cancelled(context)
        || context.workers == nullptr || worker >= context.workers->size()) {
        context.failed.store(true, std::memory_order_relaxed);
        return;
    }
    try {
        ParsedProgram parsed{};
        parsed.program.rootTag = tag;
        Parser parser(bytes, 0, parsed.inputs, parsed.writes);
        parsed.graphParsed = parser.parse(parsed.program);
        if (!parsed.graphParsed) {
            parsed.inputs.clear();
            parsed.writes.clear();
            parsed.program = {};
            parsed.program.rootTag = tag;
        }
        (*context.workers)[worker].push_back(std::move(parsed));
    } catch (...) {
        context.failed.store(true, std::memory_order_relaxed);
    }
}

} // namespace

/** Builds every scanned root and all safely recoverable channel and owner edges. */
bool build(const reader::Source& source,
           std::span<const std::uint32_t> actorTags,
           CancelProbe cancel,
           void* cancelContext,
           Snapshot& output) noexcept {
    output = {};
    if (source.directory.empty() || source.keys == nullptr) {
        return false;
    }
    try {
        std::vector<std::uint32_t> tags{};
        reader::ScanResult scan{};
        if (!reader::scan_class(source, kRootClass, &collect_tag, &tags, scan)
            || (cancel != nullptr && cancel(cancelContext))) {
            output = {};
            return false;
        }
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
        if (tags.size() > kMaximumRows) {
            return false;
        }
        std::vector<std::vector<ParsedProgram>> workers(parallel::worker_count());
        BuildContext context{cancel, cancelContext, &workers};
        bool read = false;
        {
            const parallel::ScopedRead lease{};
            read = parallel::read_tags(source, tags, &collect, &context);
        }
        if (!read || context.failed.load(std::memory_order_relaxed)) {
            return false;
        }
        std::vector<ParsedProgram> parsed{};
        for (auto& worker : workers) {
            for (ParsedProgram& row : worker) {
                parsed.push_back(std::move(row));
            }
        }
        std::sort(parsed.begin(), parsed.end(), [](const auto& first, const auto& second) {
            return first.program.rootTag < second.program.rootTag;
        });
        std::size_t parsedIndex = 0;
        for (const std::uint32_t tag : tags) {
            ParsedProgram missing{};
            missing.program.rootTag = tag;
            ParsedProgram* row = &missing;
            if (parsedIndex < parsed.size() && parsed[parsedIndex].program.rootTag == tag) {
                row = &parsed[parsedIndex++];
            }
            if (output.inputs.size() > kMaximumRows - row->inputs.size()
                || output.writes.size() > kMaximumRows - row->writes.size()) {
                output = {};
                return false;
            }
            const std::uint32_t programIndex = static_cast<std::uint32_t>(output.programs.size());
            row->program.firstInput = static_cast<std::uint32_t>(output.inputs.size());
            row->program.firstWrite = static_cast<std::uint32_t>(output.writes.size());
            for (Input& input : row->inputs) {
                input.programRow = programIndex;
                output.inputs.push_back(input);
            }
            for (ChannelWrite& write : row->writes) {
                write.programRow = programIndex;
                output.writes.push_back(write);
            }
            output.programs.push_back(row->program);
            if (row->graphParsed) {
                ++output.parsedPrograms;
            } else if (row == &missing) {
                ++output.unreadPrograms;
            } else {
                ++output.unparsedPrograms;
            }
        }
        if (!build_owners(source, actorTags, cancel, cancelContext, output)) {
            output = {};
            return false;
        }
        output.ready = true;
        log_summary(output);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::behavior_inventory
