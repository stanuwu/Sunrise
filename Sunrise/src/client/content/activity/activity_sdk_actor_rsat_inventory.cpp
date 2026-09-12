#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/reader/parallel.h"
#include "activity_sdk_actor_rsat_inventory_internal.h"
#include "activity_sdk_actor_sequences.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace {

/** Heap-owned package reader storage is always closed before release. */
class ScratchOwner final {
public:
    ScratchOwner() noexcept : value_(new(std::nothrow) reader::Scratch()) {}

    ~ScratchOwner() noexcept {
        if (value_ != nullptr) {
            reader::close_files(*value_);
        }
    }

    ScratchOwner(const ScratchOwner&) = delete;
    ScratchOwner& operator=(const ScratchOwner&) = delete;

    [[nodiscard]] reader::Scratch* get() const noexcept {
        return value_.get();
    }

private:
    std::unique_ptr<reader::Scratch> value_{};
};

/** Releases the process-wide parallel package readers after one inventory build. */
class ParallelReadOwner final {
public:
    ~ParallelReadOwner() noexcept {
        reader::parallel::release();
    }

    ParallelReadOwner() = default;
    ParallelReadOwner(const ParallelReadOwner&) = delete;
    ParallelReadOwner& operator=(const ParallelReadOwner&) = delete;
};

} // namespace

/** Builds an inventory from an already complete, sorted actor-definition tag set. */
bool build_from_tags_and_rsats(std::span<const std::uint32_t> actorTags,
                               std::span<const std::uint32_t> rsatTags,
                               ReadTag readTag,
                               void* readContext,
                               CancelProbe cancel,
                               void* cancelContext,
                               Snapshot& output,
                               std::span<const SequenceTableSource> sequenceTables) noexcept {
    output = {};
    if (actorTags.empty() || readTag == nullptr || is_cancelled(cancel, cancelContext)) {
        return false;
    }
    for (std::size_t index = 0; index < actorTags.size(); ++index) {
        if (is_absent_tag(actorTags[index])
            || (index != 0 && actorTags[index - 1U] >= actorTags[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < rsatTags.size(); ++index) {
        if (is_absent_tag(rsatTags[index])
            || (index != 0 && rsatTags[index - 1U] >= rsatTags[index])) {
            return false;
        }
    }

    try {
        BuildState state{};
        state.readTag = readTag;
        state.readContext = readContext;
        state.cancel = cancel;
        state.cancelContext = cancelContext;
        state.snapshot.actorClasses.reserve(actorTags.size());
        state.schemaSources.reserve(actorTags.size() + rsatTags.size());
        state.schemaIndexes.reserve(actorTags.size() + rsatTags.size());
        state.rsatTags.reserve(actorTags.size());
        if (!add_engine_semantics(state.snapshot)) {
            return false;
        }

        for (const auto& source : sequenceTables) {
            std::uint32_t table = format::kAbsentIndex;
            if (is_absent_tag(source.definitionTag)
                || !sequence_inventory::table(state, source, table)) {
                return false;
            }
        }

        for (std::size_t actorIndex = 0; actorIndex < actorTags.size(); ++actorIndex) {
            if (is_cancelled(cancel, cancelContext)
                || actorIndex > (std::numeric_limits<std::uint32_t>::max)()
                || state.snapshot.descriptors.size()
                       > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            const std::uint32_t actorTag = actorTags[actorIndex];
            std::vector<std::byte> actorBlob{};
            if (!readTag(readContext, actorTag, kActorClassDefinitionClass, actorBlob)
                || actorBlob.size() < kActorDefinitionMinimumSize) {
                return false;
            }

            ActorClass actor{};
            actor.definitionTag = actorTag;
            actor.descriptors.first = static_cast<std::uint32_t>(state.snapshot.descriptors.size());
            std::uint8_t objectType = 0;
            std::uint32_t behaviorConfigTag = format::kAbsentIndex;
            if (!format_actor_id(actorTag, actor.id)
                || !read_value(actorBlob, kActorNameHashOffset, actor.nameHash)
                || !read_value(
                    actorBlob, kActorAuthoredSpawnProfileOffset, actor.authoredSpawnProfile)
                || !read_value(actorBlob, kActorRsatTagOffset, actor.rsatTag)
                || !read_value(actorBlob, kActorObjectTypeOffset, objectType)) {
                return false;
            }
            if (contains(actorBlob, kActorBehaviorConfigOffset, sizeof behaviorConfigTag)
                && !read_value(actorBlob, kActorBehaviorConfigOffset, behaviorConfigTag)) {
                return false;
            }
            actor.objectType = objectType;
            ActorBehaviorProfile profile{};
            profile.actorClassIndex = static_cast<std::uint32_t>(actorIndex);
            profile.behaviorConfigTag = behaviorConfigTag;
            profile.behaviorConfigOffset = kActorBehaviorConfigOffset;
            profile.defaultFaction = 0;
            profile.flags = format::kActorBehaviorProfileExact;
            if (is_absent_tag(behaviorConfigTag)) {
                profile.behaviorConfigClass = format::kAbsentIndex;
                profile.behaviorProvenance = format::ActorSemanticProvenance::notPresent;
            } else {
                std::vector<std::byte> behaviorConfig{};
                if (readTag(readContext,
                            behaviorConfigTag,
                            format::kActorBehaviorConfigClass,
                            behaviorConfig)) {
                    profile.behaviorConfigClass = format::kActorBehaviorConfigClass;
                } else {
                    profile.behaviorConfigTag = format::kAbsentIndex;
                    profile.behaviorConfigClass = format::kAbsentIndex;
                    profile.behaviorProvenance = format::ActorSemanticProvenance::notPresent;
                }
            }
            state.snapshot.behaviorProfiles.push_back(profile);
            if (!sequence_inventory::actor(state, static_cast<std::uint32_t>(actorIndex), actorBlob)
                || !collect_state_names(state, static_cast<std::uint32_t>(actorIndex), actorBlob)) {
                return false;
            }
            if (is_absent_tag(actor.rsatTag)) {
                state.snapshot.actorClasses.push_back(actor);
                continue;
            }
            if (!state.rsatTags.emplace(actor.rsatTag).second) {
                return false;
            }

            std::vector<std::byte> rsatBlob{};
            TypedArray descriptorArray{};
            if (!readTag(readContext, actor.rsatTag, kActorRsatClass, rsatBlob)
                || !read_value(
                    rsatBlob, kActorRsatReverseDefinitionOffset, actor.rsatReverseDefinitionTag)
                || actor.rsatReverseDefinitionTag != actorTag
                || !typed_array(rsatBlob,
                                format::kActorRsatDescriptorArrayOffset,
                                format::kActorRsatDescriptorClass,
                                kDescriptorStride,
                                descriptorArray)) {
                return false;
            }
            actor.descriptorArrayOffset = format::kActorRsatDescriptorArrayOffset;
            actor.descriptorArrayRelative = descriptorArray.relative;
            actor.descriptorArrayHeaderOffset = descriptorArray.headerOffset;
            actor.descriptorArrayDataOffset = descriptorArray.dataOffset;
            actor.descriptorElementClass = descriptorArray.elementClass;
            actor.descriptors.count = descriptorArray.count;
            if (descriptorArray.count
                > (std::numeric_limits<std::uint32_t>::max)() - state.snapshot.descriptors.size()) {
                return false;
            }

            std::uint32_t tailOrdinal = 0;
            for (std::uint32_t ordinal = 0; ordinal < descriptorArray.count; ++ordinal) {
                const std::size_t offset = static_cast<std::size_t>(descriptorArray.dataOffset)
                                           + static_cast<std::size_t>(ordinal) * kDescriptorStride;
                RsatDescriptor descriptor{};
                descriptor.actorClassIndex = static_cast<std::uint32_t>(actorIndex);
                descriptor.rsatTag = actor.rsatTag;
                descriptor.descriptorOrdinal = ordinal;
                descriptor.descriptorElementClass = format::kActorRsatDescriptorClass;
                if (!format_descriptor_id(actor.rsatTag, ordinal, descriptor.id)
                    || !to_u32(offset, descriptor.descriptorOffset)) {
                    return false;
                }
                std::copy_n(
                    rsatBlob.data() + offset, descriptor.rawRow.size(), descriptor.rawRow.begin());
                std::memcpy(&descriptor.componentTag,
                            descriptor.rawRow.data(),
                            sizeof descriptor.componentTag);
                std::memcpy(&descriptor.schemaTag,
                            descriptor.rawRow.data() + 4U,
                            sizeof descriptor.schemaTag);
                std::size_t schemaSourceIndex = 0;
                if (!schema(state, descriptor.schemaTag, schemaSourceIndex)) {
                    return false;
                }
                const RsatSchema& schemaRow = state.schemaSources[schemaSourceIndex].row;
                descriptor.schemaFieldCount = schemaRow.fieldCount;
                descriptor.schemaFirstFieldRuntimeGate = schemaRow.firstFieldRuntimeGate;
                descriptor.schemaFirstFieldRawU32At10 = schemaRow.firstFieldRawU32At10;
                if ((schemaRow.flags & format::kRsatSchemaDynamicPresenceEligible) != 0) {
                    descriptor.flags |= format::kRsatDescriptorDynamicPresenceEligible;
                    descriptor.dynamicPresenceTailOrdinal = tailOrdinal++;
                }
                state.snapshot.descriptors.push_back(descriptor);
            }
            actor.dynamicPresenceTailCount = tailOrdinal;
            state.snapshot.actorClasses.push_back(actor);
        }

        std::vector<std::uint32_t> installedRsats(rsatTags.begin(), rsatTags.end());
        installedRsats.reserve(installedRsats.size() + state.rsatTags.size());
        installedRsats.insert(installedRsats.end(), state.rsatTags.begin(), state.rsatTags.end());
        std::sort(installedRsats.begin(), installedRsats.end());
        installedRsats.erase(std::unique(installedRsats.begin(), installedRsats.end()),
                             installedRsats.end());
        state.snapshot.sobjectRsats.reserve(installedRsats.size());
        for (const std::uint32_t rsatTag : installedRsats) {
            if (!sobject_rsat(state, rsatTag)) {
                return false;
            }
        }

        std::sort(state.schemaSources.begin(),
                  state.schemaSources.end(),
                  [](const SchemaSource& first, const SchemaSource& second) {
                      return first.row.schemaTag < second.row.schemaTag;
                  });
        state.schemaIndexes.clear();
        state.schemaIndexes.reserve(state.schemaSources.size());
        state.snapshot.schemas.reserve(state.schemaSources.size());
        for (std::size_t index = 0; index < state.schemaSources.size(); ++index) {
            SchemaSource& source = state.schemaSources[index];
            if (index > (std::numeric_limits<std::uint32_t>::max)()
                || state.snapshot.fields.size() > (std::numeric_limits<std::uint32_t>::max)()
                || source.fields.size() > (std::numeric_limits<std::uint32_t>::max)()
                || source.fields.size()
                       > (std::numeric_limits<std::uint32_t>::max)() - state.snapshot.fields.size()
                || !state.schemaIndexes.emplace(source.row.schemaTag, index).second) {
                return false;
            }
            source.row.fields.first = static_cast<std::uint32_t>(state.snapshot.fields.size());
            source.row.fields.count = static_cast<std::uint32_t>(source.fields.size());
            state.snapshot.schemas.push_back(source.row);
            state.snapshot.fields.insert(
                state.snapshot.fields.end(), source.fields.begin(), source.fields.end());
        }
        state.snapshot.sobjectRsatFieldBindings.reserve(state.snapshot.fields.size());
        for (std::size_t index = 0; index < state.snapshot.fields.size(); ++index) {
            if (index > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            const RsatField& field = state.snapshot.fields[index];
            SobjectRsatFieldBinding binding{};
            binding.rsatFieldIndex = static_cast<std::uint32_t>(index);
            std::memcpy(&binding.runtimeSchemaHandle,
                        field.rawRow.data() + 0x10U,
                        sizeof binding.runtimeSchemaHandle);
            std::memcpy(
                &binding.parameter14, field.rawRow.data() + 0x14U, sizeof binding.parameter14);
            std::memcpy(
                &binding.parameter18, field.rawRow.data() + 0x18U, sizeof binding.parameter18);
            std::memcpy(
                &binding.decodedOffset, field.rawRow.data() + 0x20U, sizeof binding.decodedOffset);
            binding.flags = format::kSobjectRsatFieldBindingExact;
            if (binding.runtimeSchemaHandle != format::kAbsentIndex) {
                const auto runtime =
                    std::find_if(state.snapshot.runtimeSchemas.begin(),
                                 state.snapshot.runtimeSchemas.end(),
                                 [&binding](const RuntimeSchema& row) {
                                     return row.handle == binding.runtimeSchemaHandle;
                                 });
                if (runtime == state.snapshot.runtimeSchemas.end()) {
                    return false;
                }
                binding.definitionClass = runtime->definitionClass;
                binding.codecFamilies = runtime->codecFamilies;
                binding.flags |= format::kSobjectRsatFieldBindingHasRuntimeSchema;
            }
            state.snapshot.sobjectRsatFieldBindings.push_back(binding);
        }
        for (RsatDescriptor& descriptor : state.snapshot.descriptors) {
            const auto found = state.schemaIndexes.find(descriptor.schemaTag);
            if (found == state.schemaIndexes.end()
                || found->second > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            descriptor.schemaIndex = static_cast<std::uint32_t>(found->second);
        }
        for (SobjectRsatDescriptor& descriptor : state.snapshot.sobjectRsatDescriptors) {
            const auto found = state.schemaIndexes.find(descriptor.schemaTag);
            if (found == state.schemaIndexes.end()
                || found->second > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            descriptor.schemaIndex = static_cast<std::uint32_t>(found->second);
        }

        if (!sequence_inventory::finish_names(state.snapshot)) {
            return false;
        }
        state.snapshot.complete = true;
        if (!validate(state.snapshot)) {
            return false;
        }
        output = std::move(state.snapshot);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool build_from_tags(std::span<const std::uint32_t> actorTags,
                     ReadTag readTag,
                     void* readContext,
                     CancelProbe cancel,
                     void* cancelContext,
                     Snapshot& output) noexcept {
    return build_from_tags_and_rsats(
        actorTags, {}, readTag, readContext, cancel, cancelContext, output);
}

/** Builds the complete current-build inventory for an upstream exact actor tag set. */
bool build(const reader::Source& source,
           std::span<const std::uint32_t> actorTags,
           CancelProbe cancel,
           void* cancelContext,
           Snapshot& output) noexcept {
    return build_with_rsats(source, actorTags, {}, cancel, cancelContext, output);
}

/** Builds the actor inventory from tags plus already-read RSAT rows. */
bool build_with_rsats(const reader::Source& source,
                      std::span<const std::uint32_t> actorTags,
                      std::span<const std::uint32_t> rsatTags,
                      CancelProbe cancel,
                      void* cancelContext,
                      Snapshot& output) noexcept {
    output = {};
    if (source.directory.empty() || source.keys == nullptr || actorTags.empty()
        || is_cancelled(cancel, cancelContext)) {
        return false;
    }
    try {
        ScratchOwner scratch{};
        if (scratch.get() == nullptr) {
            return false;
        }
        ParallelReadOwner parallelReads{};
        PackageReadContext context{&source, scratch.get()};
        std::vector<reader::parallel::Held> held{};
        if (!reader::parallel::read_kept(source, rsatTags, held)) {
            return false;
        }
        std::vector<std::uint32_t> schemaTags{};
        context.prefetched.reserve(held.size());
        for (const reader::parallel::Held& row : held) {
            std::uint32_t classId = 0;
            TypedArray descriptors{};
            if (!reader::read_tag_class(source, *scratch.get(), row.tag, classId)
                || classId != kActorRsatClass
                || !typed_array(row.blob,
                                format::kActorRsatDescriptorArrayOffset,
                                format::kActorRsatDescriptorClass,
                                kDescriptorStride,
                                descriptors)) {
                return false;
            }
            for (std::uint32_t ordinal = 0; ordinal < descriptors.count; ++ordinal) {
                const std::size_t offset = static_cast<std::size_t>(descriptors.dataOffset)
                                           + static_cast<std::size_t>(ordinal) * kDescriptorStride;
                std::uint32_t schemaTag = 0;
                if (!read_value(row.blob, offset + 4U, schemaTag) || is_absent_tag(schemaTag)) {
                    return false;
                }
                schemaTags.push_back(schemaTag);
            }
            PrefetchedTag cached{};
            cached.bytes.assign(row.blob.begin(), row.blob.end());
            cached.classId = kActorRsatClass;
            if (!context.prefetched.emplace(row.tag, std::move(cached)).second) {
                return false;
            }
        }
        std::sort(schemaTags.begin(), schemaTags.end());
        schemaTags.erase(std::unique(schemaTags.begin(), schemaTags.end()), schemaTags.end());
        if (is_cancelled(cancel, cancelContext)
            || !reader::parallel::read_kept(source, schemaTags, held)) {
            return false;
        }
        context.prefetched.reserve(context.prefetched.size() + held.size());
        for (const reader::parallel::Held& row : held) {
            std::uint32_t classId = 0;
            if (!reader::read_tag_class(source, *scratch.get(), row.tag, classId)
                || classId != format::kActorRsatSchemaClass) {
                return false;
            }
            PrefetchedTag cached{};
            cached.bytes.assign(row.blob.begin(), row.blob.end());
            cached.classId = format::kActorRsatSchemaClass;
            if (!context.prefetched.emplace(row.tag, std::move(cached)).second) {
                return false;
            }
        }
        std::vector<SequenceTableSource> sequenceTables;
        if (!sequence_inventory::scan_tables(source, sequenceTables)) {
            return false;
        }
        return build_from_tags_and_rsats(actorTags,
                                         rsatTags,
                                         &package_read,
                                         &context,
                                         cancel,
                                         cancelContext,
                                         output,
                                         sequenceTables);
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
