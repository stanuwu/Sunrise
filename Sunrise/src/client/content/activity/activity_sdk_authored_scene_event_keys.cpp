#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/typed_array.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808063.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "activity_sdk_authored_scene_internal.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

namespace {

namespace reader = middleware::content::packages::reader;
using generated::SceneConnectionList;
using generated::SceneGateBody;
using generated::SceneGateRow;
using ConnectionRef = generated::C8080635a;

using ArrayHeader = reader::layout::TypedArrayHeader;
/** A connection reference opens with a pointer whose second word is the list's class. */
constexpr std::size_t kConnectionClassOffset = offsetof(ConnectionRef, connectionList.word);
constexpr std::size_t kConnectionCountOffset = offsetof(ConnectionRef, ports.count);
constexpr std::size_t kConnectionListOffset = offsetof(ConnectionRef, ports.relative);
constexpr std::size_t kConnectionSingleOffset = offsetof(ConnectionRef, singlePort);
/** Port values are typed-array rows of one u32 each. */
constexpr std::size_t kPortSize = sizeof(generated::SI32Element);
/** A gate row's pointer names the body class and the body's absolute offset. */
constexpr std::size_t kGateRowClassOffset = offsetof(SceneGateRow, body.word);
constexpr std::size_t kGateRowBodyOffset = offsetof(SceneGateRow, body.offset);
/** A gate body opens with a pointer whose second word names the row class back. */
constexpr std::size_t kGateBodyClassOffset = offsetof(SceneGateBody, row.word);
constexpr std::size_t kGateKeyOffset = offsetof(SceneGateBody, key);
constexpr std::size_t kGateOrdinalOffset = offsetof(SceneGateBody, h10a7196d.ordinal);
/** A gate's own connection list names the blocks that wait on the gate. */
constexpr std::size_t kGateConnectionOffset = offsetof(SceneGateBody, connections);
constexpr std::size_t kGateListenerCountOffset = offsetof(SceneGateBody, connections.ports.count);

/** Logs a scene graph that yielded no event keys, and why. */
void log_scene_graph(const squad::DescriptorFact& descriptor,
                     std::uint32_t graphTag,
                     const char* result,
                     core::log::Level level) noexcept {
    std::array<char, 176> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_sdk_scene_graph result=%s config=0x%08X "
                                      "slot_row=%u graph=0x%08X",
                                      result,
                                      static_cast<unsigned>(descriptor.configTag),
                                      static_cast<unsigned>(descriptor.slotIndex),
                                      static_cast<unsigned>(graphTag));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            level,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/**
 * Reads the graph tag a scene resource names: the config of its first pattern component.
 * @param resource Resource bytes.
 * @param graphTag Receives the graph tag; zero when the resource has no component.
 * @return False when the component array does not read.
 */
[[nodiscard]] bool read_graph_tag(std::span<const std::byte> resource,
                                  std::uint32_t& graphTag) noexcept {
    using Component = generated::SPatternComponent;
    // Tag layout: the entity's component array descriptor.
    constexpr std::size_t kCountField = offsetof(generated::SEntity, components.count);
    constexpr std::size_t kRelativeField = offsetof(generated::SEntity, components.relative);
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    graphTag = 0;
    if (!read_value(resource, kCountField, count) || !read_value(resource, kRelativeField, relative)
        || relative <= 0
        || static_cast<std::uint64_t>(relative) > resource.size() - kRelativeField) {
        return false;
    }
    reader::TypedArrayRows rows{};
    if (!reader::read_typed_array(resource,
                                  kRelativeField + static_cast<std::size_t>(relative),
                                  count,
                                  Component::kClass,
                                  sizeof(Component),
                                  rows)) {
        return false;
    }
    return count == 0 || read_value(resource, rows.data + offsetof(Component, config), graphTag);
}

/**
 * Reads the gate table of a scene graph, held by the graph's default instance.
 * @param blob Graph bytes.
 * @param rows Receives the offset of the first table row.
 * @param count Receives the row count.
 * @return False when the field does not hold a table of gate rows that fits the graph.
 */
[[nodiscard]] bool
read_gate_table(std::span<const std::byte> blob, std::size_t& rows, std::uint64_t& count) noexcept {
    using Instance = generated::SSceneGraphInstance;
    // Tag layout: the entity config's default instance.
    constexpr std::size_t kInstanceField = offsetof(generated::SEntityConfig, defaultInstance);
    std::int64_t instanceRelative = 0;
    std::uint32_t instanceClass = 0;
    // The default-instance pointer counts from its own field and lands after the class word.
    if (!read_value(blob, kInstanceField, instanceRelative) || instanceRelative <= 0
        || static_cast<std::uint64_t>(instanceRelative) > blob.size() - kInstanceField) {
        return false;
    }
    const std::size_t instance = kInstanceField + static_cast<std::size_t>(instanceRelative);
    if (instance < reader::layout::kClassWordSize
        || !read_value(blob, instance - reader::layout::kClassWordSize, instanceClass)
        || instanceClass != Instance::kClass) {
        return false;
    }
    const std::size_t field = instance + offsetof(Instance, gateTable);
    std::int64_t relative = 0;
    const std::size_t pointer = field + reader::layout::kArrayFieldRelative;
    if (!read_value(blob, field, count) || !read_value(blob, pointer, relative) || relative <= 0
        || static_cast<std::uint64_t>(relative) > blob.size() - pointer) {
        return false;
    }
    // The pointer is relative to its own field and lands on the count of the array header.
    const std::size_t header = pointer + static_cast<std::size_t>(relative);
    reader::TypedArrayRows table{};
    if (!reader::read_typed_array(
            blob, header, count, SceneGateRow::kClass, sizeof(SceneGateRow), table)) {
        return false;
    }
    rows = table.data;
    return true;
}

/**
 * Collects every port a graph block fires, ascending and unique.
 * @param blob Graph bytes.
 * @param graphTag Tag every connection list of the graph opens with.
 * @param gateLists Offsets of the gates' own connection lists, which name waiting blocks instead.
 * @param output Receives the fired ports.
 * @return False when a connection list does not fit the graph.
 */
[[nodiscard]] bool fired_ports(std::span<const std::byte> blob,
                               std::uint32_t graphTag,
                               std::span<const std::size_t> gateLists,
                               std::vector<std::uint32_t>& output) {
    output.clear();
    for (std::size_t offset = 0;
         contains(blob, offset, kConnectionSingleOffset + sizeof(ConnectionRef::singlePort));
         offset += sizeof(std::uint32_t)) {
        std::uint32_t owner = 0;
        std::uint32_t classId = 0;
        if (!read_value(blob, offset, owner) || owner != graphTag
            || !read_value(blob, offset + kConnectionClassOffset, classId)
            || classId != SceneConnectionList::kClass
            || std::find(gateLists.begin(), gateLists.end(), offset) != gateLists.end()) {
            continue;
        }
        std::int32_t single = 0;
        std::uint64_t count = 0;
        std::int64_t relative = 0;
        const std::size_t field = offset + kConnectionListOffset;
        if (!read_value(blob, offset + kConnectionSingleOffset, single)
            || !read_value(blob, offset + kConnectionCountOffset, count)
            || !read_value(blob, field, relative)) {
            return false;
        }
        // One target sits inline; -1 means the list carries them.
        if (single != -1) {
            output.push_back(static_cast<std::uint32_t>(single));
            continue;
        }
        if (count == 0) {
            continue;
        }
        const auto header = static_cast<std::int64_t>(field) + relative;
        std::uint64_t repeated = 0;
        std::uint32_t elementClass = 0;
        if (header < 0 || !read_value(blob, static_cast<std::size_t>(header), repeated)
            || repeated != count
            || !read_value(
                blob, static_cast<std::size_t>(header) + ArrayHeader::kClass, elementClass)
            || elementClass != generated::SI32Element::kClass || count > blob.size() / kPortSize) {
            return false;
        }
        const std::size_t values = static_cast<std::size_t>(header) + ArrayHeader::kSize;
        for (std::uint64_t index = 0; index < count; ++index) {
            std::uint32_t port = 0;
            if (!read_value(blob, values + static_cast<std::size_t>(index) * kPortSize, port)) {
                return false;
            }
            output.push_back(port);
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return true;
}

} // namespace

/**
 * Follows a scene resource to its event graph and appends one row per gate.
 * A graph that cannot be read, or one gate that is not a gate element, leaves the scene with no
 * keys and is logged. Only a topology inconsistency fails the build.
 */
bool collect_event_keys(const topology::Snapshot& topology,
                        const squad::DescriptorFact& descriptor,
                        squad::TagReader reader,
                        void* readerContext,
                        PackageCache& cache,
                        std::uint32_t resourceTag,
                        std::span<const std::byte> resource,
                        std::vector<EventKey>& output) {
    const std::size_t start = output.size();
    std::uint32_t graphTag = 0;
    if (!read_graph_tag(resource, graphTag) || graphTag == 0 || graphTag == format::kAbsentIndex) {
        log_scene_graph(descriptor, graphTag, "unreferenced", core::log::Level::debug);
        return true;
    }
    const PackageRow* graph = nullptr;
    if (!package_row(reader, readerContext, graphTag, cache, graph) || graph == nullptr) {
        log_scene_graph(descriptor, graphTag, "unreadable", core::log::Level::warn);
        return true;
    }
    if (graph->classId != generated::SEntityConfig::kClass) {
        log_scene_graph(descriptor, graphTag, "graph_class", core::log::Level::warn);
        return true;
    }
    const auto blob = std::span(graph->bytes);
    std::size_t rows = 0;
    std::uint64_t count = 0;
    if (!read_gate_table(blob, rows, count)) {
        log_scene_graph(descriptor, graphTag, "gate_table", core::log::Level::warn);
        return true;
    }
    if (count > format::kAuthoredSceneGateCapacity) {
        log_scene_graph(descriptor, graphTag, "capacity", core::log::Level::warn);
        return true;
    }
    std::vector<std::size_t> gateLists;
    for (std::uint64_t gate = 0; gate < count; ++gate) {
        const std::size_t tableRow = rows + static_cast<std::size_t>(gate) * sizeof(SceneGateRow);
        std::uint32_t rowOwner = 0;
        std::uint32_t rowClass = 0;
        std::uint64_t body = 0;
        // A row names its body by absolute offset; the body names the row class back.
        if (!read_value(blob, tableRow, rowOwner)
            || !read_value(blob, tableRow + kGateRowClassOffset, rowClass)
            || !read_value(blob, tableRow + kGateRowBodyOffset, body) || rowOwner != graphTag
            || rowClass != SceneGateBody::kClass || body > blob.size() - sizeof(SceneGateBody)) {
            output.resize(start);
            log_scene_graph(descriptor, graphTag, "gate_row", core::log::Level::warn);
            return true;
        }
        const auto element = static_cast<std::size_t>(body);
        std::uint32_t owner = 0;
        std::uint32_t bodyClass = 0;
        std::uint32_t key = 0;
        std::int32_t ordinal = 0;
        std::uint64_t listeners = 0;
        if (!read_value(blob, element, owner)
            || !read_value(blob, element + kGateBodyClassOffset, bodyClass)
            || !read_value(blob, element + kGateKeyOffset, key)
            || !read_value(blob, element + kGateOrdinalOffset, ordinal)
            || !read_value(blob, element + kGateListenerCountOffset, listeners) || owner != graphTag
            || bodyClass != SceneGateRow::kClass || key == 0 || key == format::kAbsentIndex) {
            output.resize(start);
            log_scene_graph(descriptor, graphTag, "gate", core::log::Level::warn);
            return true;
        }
        EventKey row{};
        if (!event_key_id(
                topology, descriptor, graphTag, static_cast<std::uint32_t>(element), row.id)) {
            return false;
        }
        row.slotIndex = descriptor.slotIndex;
        row.resourceTag = resourceTag;
        row.graphTag = graphTag;
        row.gateOffset = static_cast<std::uint32_t>(element);
        row.ordinal = ordinal;
        row.key = key;
        row.flags = format::kAuthoredSceneEventKeyExact
                    | (listeners != 0 ? format::kAuthoredSceneEventKeyListened : 0U);
        output.push_back(row);
        gateLists.push_back(element + kGateConnectionOffset);
    }
    std::vector<std::uint32_t> fired;
    if (!fired_ports(blob, graphTag, gateLists, fired)) {
        output.resize(start);
        log_scene_graph(descriptor, graphTag, "connections", core::log::Level::warn);
        return true;
    }
    // Port k is gate ordinal k, so a block firing port k fires that gate.
    for (std::size_t index = start; index < output.size(); ++index) {
        EventKey& row = output[index];
        if (row.ordinal >= 0
            && std::binary_search(
                fired.begin(), fired.end(), static_cast<std::uint32_t>(row.ordinal))) {
            row.flags |= format::kAuthoredSceneEventKeyFired;
        }
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
