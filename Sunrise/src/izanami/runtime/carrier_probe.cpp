#include "carrier_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../../client/content/items/packages/internal.h"
#include "../../core/logging/log.h"
#include "../../middleware/content/packages/reader/reader.h"
#include "../../middleware/content/packages/tables/roster_intersection.h"
#include "../../middleware/content/packages/tables/scenario_walk.h"
#include "../../state/build_data/runtime.h"
#include "../../state/content/content_catalog.h"

namespace sunrise::izanami::runtime::carrier_probe {
namespace {

namespace item_packages = client::content::items::packages;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Storage for nested tag reads; each walk slot must remain live until its consumer returns. */
struct ReadContext {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    std::array<std::vector<std::byte>, static_cast<std::size_t>(tables::ReadSlot::count)> bytes{};
};

/** Extra placement facts that are not part of the walk's aggregate result. */
struct PlacementSummary {
    std::vector<std::uint32_t> objectTags{};
    std::vector<std::uint32_t> rosterTags{};
    std::vector<std::uint32_t> registryTags{};
};

/** One readable definition directly named by the selected slice entry. */
struct SliceReference {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint32_t offset{};
    std::size_t size{};
};

/** One pending edge in a bounded map-definition graph walk. */
struct MapNode {
    std::uint32_t tag{};
    std::uint32_t parent{};
    std::uint32_t offset{};
    std::uint8_t depth{};
};

/** One resource class and its frequency inside a map data table. */
struct MapResourceClass {
    std::uint32_t classId{};
    std::size_t count{};
};

/** The root graph is diagnostic; a malformed or cyclic package cannot make it unbounded. */
constexpr std::size_t kMapNodeCapacity = 512;
constexpr std::uint8_t kMapGraphDepth = 5;
/** Shadowkeep map data tables use this class and a fixed 0x90-byte placement row. */
constexpr std::uint32_t kMapDataTableClass = 0x808099D6;
constexpr std::size_t kMapTableArrayOffset = 0x8;
constexpr std::size_t kMapEntryStride = 0x90;
constexpr std::size_t kMapEntryResourcePointer = 0x78;
constexpr std::size_t kMaximumMapEntries = 4096;
constexpr std::uint32_t kStaticMapResourceClass = 0x808071B3;
constexpr std::uint32_t kTerrainResourceClass = 0x8080714B;
constexpr std::size_t kMapEntryTranslation = 0x20;
constexpr std::size_t kStaticMapParentOffset = 0x10;
constexpr std::size_t kTerrainTagOffset = 0x18;
constexpr std::size_t kTerrainBoundsTagOffset = 0x1C;

/** Reads one nested tag into storage owned by its walk slot. */
bool read_tag(void* context,
              tables::ReadSlot slot,
              std::uint32_t tag,
              std::span<const std::byte>& blob) noexcept {
    blob = {};
    auto& state = *static_cast<ReadContext*>(context);
    const std::size_t index = static_cast<std::size_t>(slot);
    if (state.source == nullptr || state.scratch == nullptr || index >= state.bytes.size()
        || !reader::read_tag(*state.source, *state.scratch, tag, state.bytes[index])) {
        return false;
    }
    blob = state.bytes[index];
    return true;
}

/** Collects unique registry, placement, and player-roster object tags. */
bool visit_placement(void* context, const tables::Placement& placement) noexcept {
    auto& summary = *static_cast<PlacementSummary*>(context);
    summary.objectTags.push_back(placement.objectTag);
    summary.registryTags.push_back(placement.registryTag);
    if (tables::carries_roster_slot(placement.objectBytes)) {
        summary.rosterTags.push_back(placement.objectTag);
    }
    return true;
}

/** Sorts and removes duplicate tags from one collected bank. */
void unique(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

/** Reports one direct slice-entry dependency without dumping proprietary definition bytes. */
void report_reference(std::string_view scenario,
                      std::uint32_t entryTag,
                      const SliceReference& reference) noexcept {
    std::array<char, 256> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=izanami_carrier_ref scenario=%.*s entry=0x%08X offset=0x%X tag=0x%08X "
                      "class=0x%08X bytes=%zu",
                      static_cast<int>(scenario.size()),
                      scenario.data(),
                      static_cast<unsigned>(entryTag),
                      static_cast<unsigned>(reference.offset),
                      static_cast<unsigned>(reference.tag),
                      static_cast<unsigned>(reference.classId),
                      reference.size);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one readable node reached from the installed map root. */
void report_map_node(std::string_view mapName,
                     const MapNode& node,
                     std::uint32_t classId,
                     std::size_t size) noexcept {
    std::array<char, 280> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=izanami_map_node map=%.*s depth=%u parent=0x%08X offset=0x%X tag=0x%08X "
                      "class=0x%08X bytes=%zu",
                      static_cast<int>(mapName.size()),
                      mapName.data(),
                      static_cast<unsigned>(node.depth),
                      static_cast<unsigned>(node.parent),
                      static_cast<unsigned>(node.offset),
                      static_cast<unsigned>(node.tag),
                      static_cast<unsigned>(classId),
                      size);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Resolves a Tiger relative pointer without allowing signed or size overflow. */
[[nodiscard]] bool relative_target(std::size_t base,
                                   std::int64_t relative,
                                   std::size_t limit,
                                   std::size_t& target) noexcept {
    if (base > limit) {
        return false;
    }
    if (relative < 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-(relative + 1)) + 1U;
        if (magnitude > base) {
            return false;
        }
        target = base - static_cast<std::size_t>(magnitude);
    } else {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(relative);
        if (magnitude > limit - base) {
            return false;
        }
        target = base + static_cast<std::size_t>(magnitude);
    }
    return target <= limit;
}

/** Reports the resource-class composition of one native map data table. */
void inspect_map_table(std::uint32_t tableTag,
                       std::span<const std::byte> bytes,
                       const reader::Source& source,
                       reader::Scratch& scratch) noexcept {
    constexpr std::size_t kArrayCountOffset = kMapTableArrayOffset;
    constexpr std::size_t kArrayPointerOffset = kMapTableArrayOffset + sizeof(std::uint64_t);
    constexpr std::size_t kArrayPointerBias = 0x10;
    if (bytes.size() < kArrayPointerOffset + sizeof(std::int64_t)) {
        return;
    }

    std::uint32_t count = 0;
    std::int64_t relative = 0;
    std::memcpy(&count, bytes.data() + kArrayCountOffset, sizeof count);
    std::memcpy(&relative, bytes.data() + kArrayPointerOffset, sizeof relative);
    std::size_t entriesOffset = 0;
    const std::size_t pointerBase = kArrayPointerOffset;
    if (count > kMaximumMapEntries || pointerBase > bytes.size() - kArrayPointerBias
        || !relative_target(pointerBase + kArrayPointerBias, relative, bytes.size(), entriesOffset)
        || count > (bytes.size() - entriesOffset) / kMapEntryStride) {
        std::array<char, 192> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=izanami_map_table result=invalid table=0x%08X entries=%u "
                          "bytes=%zu",
                          static_cast<unsigned>(tableTag),
                          static_cast<unsigned>(count),
                          bytes.size());
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }

    std::vector<MapResourceClass> classes{};
    std::size_t entityTags = 0;
    std::size_t resources = 0;
    std::size_t malformed = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entryOffset = entriesOffset + index * kMapEntryStride;
        std::uint32_t entityTag = 0;
        std::memcpy(&entityTag, bytes.data() + entryOffset, sizeof entityTag);
        if (entityTag >= tables::kTagLowerBound && entityTag < tables::kTagUpperBound) {
            ++entityTags;
        }

        std::int64_t resourceRelative = 0;
        const std::size_t pointerOffset = entryOffset + kMapEntryResourcePointer;
        std::memcpy(&resourceRelative, bytes.data() + pointerOffset, sizeof resourceRelative);
        if (resourceRelative == 0) {
            continue;
        }
        std::size_t resourceOffset = 0;
        if (!relative_target(pointerOffset, resourceRelative, bytes.size(), resourceOffset)
            || resourceOffset < sizeof(std::uint32_t)) {
            ++malformed;
            continue;
        }
        std::uint32_t resourceClass = 0;
        std::memcpy(&resourceClass,
                    bytes.data() + resourceOffset - sizeof(resourceClass),
                    sizeof resourceClass);
        if (resourceClass == kStaticMapResourceClass && resourceOffset <= bytes.size()
            && kStaticMapParentOffset + sizeof(std::uint32_t) <= bytes.size() - resourceOffset) {
            std::uint32_t staticMapParent = 0;
            std::array<float, 3> translation{};
            std::memcpy(&staticMapParent,
                        bytes.data() + resourceOffset + kStaticMapParentOffset,
                        sizeof staticMapParent);
            std::memcpy(translation.data(),
                        bytes.data() + entryOffset + kMapEntryTranslation,
                        sizeof translation);
            std::array<char, 256> candidateLine{};
            const int candidateWritten =
                std::snprintf(candidateLine.data(),
                              candidateLine.size(),
                              "ev=izanami_static_candidate table=0x%08X entry=%zu entity=0x%08X "
                              "parent=0x%08X position=(%.3f,%.3f,%.3f)",
                              static_cast<unsigned>(tableTag),
                              index,
                              static_cast<unsigned>(entityTag),
                              static_cast<unsigned>(staticMapParent),
                              translation[0],
                              translation[1],
                              translation[2]);
            if (candidateWritten > 0) {
                core::log::write(
                    core::log::Channel::client,
                    core::log::Level::info,
                    {candidateLine.data(), static_cast<std::size_t>(candidateWritten)});
            }

            std::vector<std::byte> parentBytes{};
            std::uint32_t parentClass = 0;
            std::uint32_t staticMapTag = 0;
            std::vector<std::byte> staticMapBytes{};
            std::uint32_t staticMapClass = 0;
            if (reader::read_tag(source, scratch, staticMapParent, parentBytes, parentClass)
                && parentBytes.size() >= 0xC) {
                std::memcpy(&staticMapTag, parentBytes.data() + 0x8, sizeof staticMapTag);
                if (!reader::read_tag(
                        source, scratch, staticMapTag, staticMapBytes, staticMapClass)) {
                    staticMapBytes.clear();
                    staticMapClass = 0;
                }
            }
            const int payloadWritten =
                std::snprintf(candidateLine.data(),
                              candidateLine.size(),
                              "ev=izanami_static_payload table=0x%08X parent=0x%08X "
                              "parent_class=0x%08X parent_bytes=%zu data=0x%08X "
                              "data_class=0x%08X data_bytes=%zu",
                              static_cast<unsigned>(tableTag),
                              static_cast<unsigned>(staticMapParent),
                              static_cast<unsigned>(parentClass),
                              parentBytes.size(),
                              static_cast<unsigned>(staticMapTag),
                              static_cast<unsigned>(staticMapClass),
                              staticMapBytes.size());
            if (payloadWritten > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {candidateLine.data(), static_cast<std::size_t>(payloadWritten)});
            }
        } else if (resourceClass == kTerrainResourceClass && resourceOffset <= bytes.size()
                   && kTerrainBoundsTagOffset + sizeof(std::uint32_t)
                          <= bytes.size() - resourceOffset) {
            std::uint32_t terrainTag = 0;
            std::uint32_t boundsTag = 0;
            std::array<float, 3> translation{};
            std::memcpy(
                &terrainTag, bytes.data() + resourceOffset + kTerrainTagOffset, sizeof terrainTag);
            std::memcpy(&boundsTag,
                        bytes.data() + resourceOffset + kTerrainBoundsTagOffset,
                        sizeof boundsTag);
            std::memcpy(translation.data(),
                        bytes.data() + entryOffset + kMapEntryTranslation,
                        sizeof translation);
            std::array<char, 280> candidateLine{};
            const int candidateWritten =
                std::snprintf(candidateLine.data(),
                              candidateLine.size(),
                              "ev=izanami_terrain_candidate table=0x%08X entry=%zu entity=0x%08X "
                              "terrain=0x%08X bounds=0x%08X position=(%.3f,%.3f,%.3f)",
                              static_cast<unsigned>(tableTag),
                              index,
                              static_cast<unsigned>(entityTag),
                              static_cast<unsigned>(terrainTag),
                              static_cast<unsigned>(boundsTag),
                              translation[0],
                              translation[1],
                              translation[2]);
            if (candidateWritten > 0) {
                core::log::write(
                    core::log::Channel::client,
                    core::log::Level::info,
                    {candidateLine.data(), static_cast<std::size_t>(candidateWritten)});
            }
        }
        ++resources;
        const auto found =
            std::find_if(classes.begin(), classes.end(), [resourceClass](const auto& row) {
                return row.classId == resourceClass;
            });
        if (found == classes.end()) {
            classes.push_back(MapResourceClass{resourceClass, 1});
        } else {
            ++found->count;
        }
    }

    std::array<char, 224> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=izanami_map_table result=ok table=0x%08X entries=%u "
                                "entity_tags=%zu resources=%zu malformed=%zu bytes=%zu",
                                static_cast<unsigned>(tableTag),
                                static_cast<unsigned>(count),
                                entityTags,
                                resources,
                                malformed,
                                bytes.size());
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    for (const MapResourceClass& row : classes) {
        written = std::snprintf(line.data(),
                                line.size(),
                                "ev=izanami_map_table_class table=0x%08X class=0x%08X count=%zu",
                                static_cast<unsigned>(tableTag),
                                static_cast<unsigned>(row.classId),
                                row.count);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Walks readable tag handles from one named map root without modifying any package bytes. */
void inspect_map_graph(std::string_view mapName,
                       const reader::Source& source,
                       reader::Scratch& scratch) noexcept {
    std::array<state::content::Definition, 4> matches{};
    std::size_t matchCount = 0;
    if (!state::content::lookup(mapName, matches, matchCount) || matchCount == 0) {
        return;
    }

    std::vector<MapNode> queue{};
    std::vector<std::uint32_t> visited{};
    queue.reserve(kMapNodeCapacity);
    visited.reserve(kMapNodeCapacity);
    queue.push_back(MapNode{matches[0].tag, 0, 0, 0});
    std::vector<std::byte> bytes{};
    for (std::size_t cursor = 0; cursor < queue.size() && cursor < kMapNodeCapacity; ++cursor) {
        const MapNode node = queue[cursor];
        if (std::find(visited.begin(), visited.end(), node.tag) != visited.end()) {
            continue;
        }
        std::uint32_t classId = 0;
        if (!reader::read_tag(source, scratch, node.tag, bytes, classId)) {
            continue;
        }
        visited.push_back(node.tag);
        report_map_node(mapName, node, classId, bytes.size());
        if (classId == kMapDataTableClass) {
            inspect_map_table(node.tag, bytes, source, scratch);
        }
        if (node.depth >= kMapGraphDepth) {
            continue;
        }
        for (std::size_t offset = 0;
             offset + sizeof(std::uint32_t) <= bytes.size() && queue.size() < kMapNodeCapacity;
             offset += sizeof(std::uint32_t)) {
            std::uint32_t child = 0;
            std::memcpy(&child, bytes.data() + offset, sizeof child);
            if (child < tables::kTagLowerBound || child >= tables::kTagUpperBound
                || std::find(visited.begin(), visited.end(), child) != visited.end()) {
                continue;
            }
            queue.push_back(MapNode{child,
                                    node.tag,
                                    static_cast<std::uint32_t>(offset),
                                    static_cast<std::uint8_t>(node.depth + 1)});
        }
        const std::uint16_t packageId = static_cast<std::uint16_t>(
            (node.tag - reader::layout::kTagBase) >> reader::layout::kTagEntryBits);
        for (std::size_t offset = 0;
             offset + 2 * sizeof(std::uint32_t) + sizeof(std::uint64_t) <= bytes.size()
             && queue.size() < kMapNodeCapacity;
             offset += sizeof(std::uint32_t)) {
            std::uint32_t isHash32 = 0;
            std::uint64_t hash64 = 0;
            std::memcpy(&isHash32, bytes.data() + offset + sizeof(std::uint32_t), sizeof isHash32);
            std::memcpy(&hash64, bytes.data() + offset + 2 * sizeof(std::uint32_t), sizeof hash64);
            if (isHash32 != 0 || hash64 == 0) {
                continue;
            }
            std::uint32_t child = 0;
            std::uint32_t childClass = 0;
            if (!reader::resolve_hash64(source.directory, packageId, hash64, child, childClass)
                || child < tables::kTagLowerBound || child >= tables::kTagUpperBound
                || std::find(visited.begin(), visited.end(), child) != visited.end()) {
                continue;
            }
            queue.push_back(MapNode{child,
                                    node.tag,
                                    static_cast<std::uint32_t>(offset),
                                    static_cast<std::uint8_t>(node.depth + 1)});
        }
    }
}

/**
 * Resolves the first live slice entry and identifies all of its directly readable tag handles.
 * Those classes distinguish static-world, mission, audio, lighting, and object-registry payloads.
 */
void inspect_slice_references(std::string_view scenarioName,
                              std::span<const std::byte> scenarioBytes,
                              const reader::Source& source,
                              reader::Scratch& scratch) noexcept {
    tables::Array bubbles{};
    tables::Bubble bubble{};
    tables::SliceState state{};
    if (!tables::scenario_bubbles(scenarioBytes, bubbles)
        || !tables::bubble_at(scenarioBytes, bubbles, 0, bubble) || bubble.stateCount == 0
        || !tables::slice_state_at(scenarioBytes, bubble, 0, state)) {
        return;
    }

    std::vector<std::byte> entryBytes{};
    std::uint32_t entryClass = 0;
    if (!reader::read_tag(source, scratch, state.entryTag, entryBytes, entryClass)) {
        return;
    }

    std::vector<SliceReference> references{};
    std::vector<std::byte> referencedBytes{};
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= entryBytes.size();
         offset += sizeof(std::uint32_t)) {
        std::uint32_t tag = 0;
        std::memcpy(&tag, entryBytes.data() + offset, sizeof tag);
        if (tag < tables::kTagLowerBound || tag >= tables::kTagUpperBound) {
            continue;
        }
        const auto duplicate = std::find_if(references.begin(),
                                            references.end(),
                                            [tag](const auto& row) { return row.tag == tag; });
        if (duplicate != references.end()) {
            continue;
        }
        std::uint32_t classId = 0;
        if (!reader::read_tag(source, scratch, tag, referencedBytes, classId)) {
            continue;
        }
        references.push_back(SliceReference{
            tag, classId, static_cast<std::uint32_t>(offset), referencedBytes.size()});
    }
    for (const SliceReference& reference : references) {
        report_reference(scenarioName, state.entryTag, reference);
    }
}

/** Reports one terminal probe result without exposing package key material. */
void report(std::string_view scenario,
            std::string_view result,
            std::uint32_t scenarioTag,
            const tables::WalkResult& walk,
            const PlacementSummary& summary) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=izanami_carrier_probe result=%.*s scenario=%.*s tag=0x%08X bubbles=%llu "
        "states=%llu registries=%zu placements=%llu objects=%zu roster_objects=%zu unresolved=%llu",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(scenario.size()),
        scenario.data(),
        static_cast<unsigned>(scenarioTag),
        static_cast<unsigned long long>(walk.bubbles),
        static_cast<unsigned long long>(walk.states),
        summary.registryTags.size(),
        static_cast<unsigned long long>(walk.placements),
        summary.objectTags.size(),
        summary.rosterTags.size(),
        static_cast<unsigned long long>(walk.unresolved));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == "ok" ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Reads and classifies one destination's placed-object registries. */
void inspect(std::string_view scenarioName, std::string_view mapRootName) noexcept {
    state::build_data::scenarios::Definition scenario{};
    if (!state::build_data::find_scenario_layout(scenarioName, scenario)) {
        report(scenarioName, "scenario_missing", 0, {}, {});
        return;
    }

    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!item_packages::collect_keys(keys) || !item_packages::package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report(scenarioName, "reader_unavailable", scenario.tag, {}, {});
        return;
    }

    const reader::Source source{directory.chars.data(), &keys};
    auto scratch = std::make_unique<reader::Scratch>();
    std::vector<std::byte> scenarioBytes{};
    tables::WalkResult walk{};
    PlacementSummary summary{};
    bool complete = false;
    if (scratch != nullptr && reader::read_tag(source, *scratch, scenario.tag, scenarioBytes)) {
        inspect_slice_references(scenarioName, scenarioBytes, source, *scratch);
        inspect_map_graph(mapRootName, source, *scratch);
        ReadContext reads{&source, scratch.get()};
        complete = tables::walk_scenario(
            scenarioBytes, &read_tag, &reads, &visit_placement, &summary, walk);
        reader::close_files(*scratch);
    }
    SecureZeroMemory(&keys, sizeof keys);
    unique(summary.objectTags);
    unique(summary.rosterTags);
    unique(summary.registryTags);
    report(scenarioName,
           complete && walk.unresolved == 0 ? "ok" : "incomplete",
           scenario.tag,
           walk,
           summary);
}

} // namespace sunrise::izanami::runtime::carrier_probe
