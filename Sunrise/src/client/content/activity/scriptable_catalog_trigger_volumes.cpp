#include "scriptable_catalog_trigger_volumes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/trigger_volume_reader.h"
#include "scriptable_catalog_inline_names.h"
#include "scriptable_catalog_trigger_volume_links.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace catalog = state::build_data::scriptables;
namespace package_reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

// Fixed capacities bound one scenario's trigger tables, prisms and components.
constexpr std::size_t kTableCapacity = 65'536;
constexpr std::size_t kOwnerCapacity = 262'144;
constexpr std::size_t kInstanceCapacity = 262'144;
constexpr std::size_t kVertexCapacity = 1'048'576;
constexpr std::size_t kTriangleCapacity = 1'048'576;
constexpr std::size_t kComponentCapacity = 4'096;
// Authored bounds must agree within this absolute tolerance.
constexpr float kBoundsTolerance = 0.0001F;

struct BuildContext final {
    const package_reader::Source* source{};
    package_reader::Scratch* scratch{};
    catalog::Snapshot* output{};
    TriggerVolumeCancelCheck cancel{};
    std::vector<std::byte> configBytes{};
    bool failed{};
};

[[nodiscard]] bool cancelled(const BuildContext& context) noexcept {
    return context.cancel != nullptr && context.cancel();
}

/** Adds one bounded diagnostic count without wrapping. */
void add_dropped(std::uint64_t& destination, std::size_t count) noexcept {
    const std::uint64_t available = (std::numeric_limits<std::uint64_t>::max)() - destination;
    destination += (std::min)(available, static_cast<std::uint64_t>(count));
}

/** Marks one recoverable package, layout, or geometry validation failure. */
void mark_unresolved(BuildContext& context) noexcept {
    ++context.output->triggerVolumeDiagnostics.unresolvedReads;
    context.output->triggerVolumeDiagnostics.complete = false;
}

[[nodiscard]] bool same_identity(const tables::TriggerVolumeIdentity& left,
                                 const tables::TriggerVolumeIdentity& right) noexcept {
    return left.key == right.key && left.type == right.type && left.index == right.index;
}

/** Accepts the package tolerance and the one-ULP rounding used by stored derived bounds. */
[[nodiscard]] bool near(float left, float right) noexcept {
    if (!std::isfinite(left) || !std::isfinite(right)) {
        return false;
    }
    return std::fabs(left - right) <= kBoundsTolerance || std::nextafter(left, right) == right;
}

/** Reads and validates one row's complete world prism without publishing partial arrays. */
[[nodiscard]] bool read_geometry(BuildContext& context,
                                 const tables::TriggerVolumeRow& source,
                                 std::vector<catalog::TriggerVolumeVertex>& vertices,
                                 std::vector<catalog::TriggerVolumeTriangle>& triangles) {
    vertices.clear();
    triangles.clear();
    vertices.reserve(static_cast<std::size_t>(source.vertices.count));
    triangles.reserve(static_cast<std::size_t>(source.triangles.count));
    std::array<float, 3> minimum{
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
    };
    std::array<float, 3> maximum{
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)(),
        -(std::numeric_limits<float>::max)(),
    };
    for (std::size_t index = 0; index < source.vertices.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        catalog::TriggerVolumeVertex vertex{};
        if (!tables::trigger_volume_vertex_at(
                context.configBytes, source.vertices, index, vertex.value)) {
            return false;
        }
        const float top = vertex.value[2] + source.extrusion;
        if (!std::isfinite(top)) {
            return false;
        }
        minimum[0] = (std::min)(minimum[0], vertex.value[0]);
        minimum[1] = (std::min)(minimum[1], vertex.value[1]);
        minimum[2] = (std::min)(minimum[2], vertex.value[2]);
        maximum[0] = (std::max)(maximum[0], vertex.value[0]);
        maximum[1] = (std::max)(maximum[1], vertex.value[1]);
        maximum[2] = (std::max)(maximum[2], top);
        vertices.push_back(vertex);
    }
    for (std::size_t index = 0; index < source.triangles.count; ++index) {
        if (cancelled(context)) {
            context.failed = true;
            return false;
        }
        catalog::TriggerVolumeTriangle triangle{};
        if (!tables::trigger_volume_triangle_at(
                context.configBytes, source.triangles, index, triangle.indices)
            || std::any_of(
                triangle.indices.begin(), triangle.indices.end(), [&vertices](std::uint8_t value) {
                    return static_cast<std::size_t>(value) >= vertices.size();
                })) {
            return false;
        }
        triangles.push_back(triangle);
    }
    for (std::size_t lane = 0; lane < minimum.size(); ++lane) {
        if (!near(minimum[lane], source.minimum[lane])
            || !near(maximum[lane], source.maximum[lane])) {
            return false;
        }
    }
    return true;
}

/** Appends one exact matching row, retaining an incomplete identity when validation fails. */
void append_instance(BuildContext& context,
                     std::uint32_t tableRow,
                     std::uint32_t authoredRow,
                     const tables::TriggerVolumeRoot& root,
                     const tables::TriggerVolumeIdentity& identity,
                     std::vector<catalog::TriggerVolumeVertex>& vertices,
                     std::vector<catalog::TriggerVolumeTriangle>& triangles,
                     bool& tableComplete) {
    auto& diagnostics = context.output->triggerVolumeDiagnostics;
    if (context.output->triggerVolumeInstances.size() >= kInstanceCapacity) {
        ++diagnostics.droppedInstances;
        diagnostics.complete = false;
        tableComplete = false;
        return;
    }
    catalog::TriggerVolumeInstance destination{};
    destination.tableRow = tableRow;
    destination.authoredRowIndex = authoredRow;
    tables::TriggerVolumeRow source{};
    const bool rowRead =
        tables::trigger_volume_row_at(context.configBytes, root, authoredRow, source);
    bool complete = rowRead && same_identity(source.identity, identity)
                    && same_identity(source.shape.identity, identity);
    if (complete) {
        try {
            complete = read_geometry(context, source, vertices, triangles);
        } catch (...) {
            context.failed = true;
            return;
        }
    }
    if (!complete) {
        mark_unresolved(context);
        tableComplete = false;
        context.output->triggerVolumeInstances.push_back(destination);
        return;
    }
    if (vertices.size() > kVertexCapacity - context.output->triggerVolumeVertices.size()
        || triangles.size() > kTriangleCapacity - context.output->triggerVolumeTriangles.size()) {
        add_dropped(diagnostics.droppedVertices, vertices.size());
        add_dropped(diagnostics.droppedTriangles, triangles.size());
        diagnostics.complete = false;
        tableComplete = false;
        context.output->triggerVolumeInstances.push_back(destination);
        return;
    }
    destination.classDefinitionTag = source.classDefinitionTag;
    destination.shapeResourceTag = source.shape.resourceTag;
    destination.shapeReferenceWord = source.shape.referenceWord;
    destination.shapeIndex = source.shape.shapeIndex;
    destination.firstVertex =
        static_cast<std::uint32_t>(context.output->triggerVolumeVertices.size());
    destination.vertexCount = static_cast<std::uint32_t>(vertices.size());
    destination.firstTriangle =
        static_cast<std::uint32_t>(context.output->triggerVolumeTriangles.size());
    destination.triangleCount = static_cast<std::uint32_t>(triangles.size());
    destination.flags = source.flags;
    destination.rotation = source.rotation;
    destination.position = source.position;
    destination.minimum = source.minimum;
    destination.maximum = source.maximum;
    destination.extrusion = source.extrusion;
    destination.active = source.active;
    destination.complete = true;
    context.output->triggerVolumeVertices.insert(
        context.output->triggerVolumeVertices.end(), vertices.begin(), vertices.end());
    context.output->triggerVolumeTriangles.insert(
        context.output->triggerVolumeTriangles.end(), triangles.begin(), triangles.end());
    context.output->triggerVolumeInstances.push_back(destination);
}

/** Appends one object occurrence's exact slot ownership of a parsed table root. */
void append_owner(BuildContext& context,
                  std::uint32_t tableRow,
                  std::uint32_t objectRow,
                  const tables::TriggerVolumeIdentity& identity) {
    if (objectRow >= context.output->objects.size()) {
        mark_unresolved(context);
        return;
    }
    const catalog::Object& object = context.output->objects[objectRow];
    if (object.registryKey != identity.key) {
        return;
    }
    catalog::TriggerVolumeOwner owner{};
    owner.tableRow = tableRow;
    owner.objectRow = objectRow;
    const std::size_t first = object.firstSlot;
    const std::size_t count = object.slotCount;
    if (first <= context.output->slots.size() && count <= context.output->slots.size() - first) {
        for (std::size_t offset = 0; offset < count; ++offset) {
            const catalog::Slot& slot = context.output->slots[first + offset];
            if (slot.slotType == identity.type && slot.slotIndex == identity.index) {
                owner.slotRow = static_cast<std::uint32_t>(first + offset);
                ++owner.slotMatchCount;
            }
        }
    }
    if (owner.slotMatchCount == 1) {
        owner.slotJoin = catalog::ReferenceJoin::exact;
    } else if (owner.slotMatchCount > 1) {
        owner.slotRow = catalog::kNoRow;
        owner.slotJoin = catalog::ReferenceJoin::ambiguous;
    }
    auto& diagnostics = context.output->triggerVolumeDiagnostics;
    if (context.output->triggerVolumeOwners.size() >= kOwnerCapacity) {
        ++diagnostics.droppedOwners;
        diagnostics.complete = false;
        return;
    }
    context.output->triggerVolumeOwners.push_back(owner);
}

/** Publishes every row identity instead of treating the component root as the only identity. */
void append_root(BuildContext& context,
                 std::span<const TriggerVolumeInput> inputs,
                 const tables::Array& components,
                 std::size_t componentIndex,
                 std::uint32_t configTag,
                 std::vector<catalog::TriggerVolumeVertex>& vertices,
                 std::vector<catalog::TriggerVolumeTriangle>& triangles) {
    auto& diagnostics = context.output->triggerVolumeDiagnostics;
    tables::TriggerVolumeRoot root{};
    if (!tables::trigger_volume_root_at(
            context.configBytes, components, componentIndex, configTag, root)) {
        mark_unresolved(context);
        return;
    }
    std::vector<std::pair<tables::TriggerVolumeIdentity, std::uint32_t>> rows{};
    rows.reserve(static_cast<std::size_t>(root.rows.count));
    for (std::size_t rowIndex = 0; rowIndex < root.rows.count; ++rowIndex) {
        if (cancelled(context)) {
            context.failed = true;
            return;
        }
        tables::TriggerVolumeIdentity identity{};
        if (!tables::trigger_volume_row_identity_at(
                context.configBytes, root, rowIndex, identity)) {
            mark_unresolved(context);
            continue;
        }
        rows.emplace_back(identity, static_cast<std::uint32_t>(rowIndex));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& first, const auto& second) noexcept {
        return std::tie(first.first.key, first.first.type, first.first.index, first.second)
               < std::tie(second.first.key, second.first.type, second.first.index, second.second);
    });
    for (std::size_t first = 0; first < rows.size();) {
        std::size_t last = first + 1;
        while (last < rows.size() && same_identity(rows[first].first, rows[last].first)) {
            ++last;
        }
        if (context.output->triggerVolumeTables.size() >= kTableCapacity) {
            add_dropped(diagnostics.droppedTables, rows.size() - first);
            diagnostics.complete = false;
            return;
        }
        const tables::TriggerVolumeIdentity identity = rows[first].first;
        catalog::TriggerVolumeTable table{};
        table.configTag = configTag;
        table.firstInstance =
            static_cast<std::uint32_t>(context.output->triggerVolumeInstances.size());
        table.registryKey = identity.key;
        table.componentOrdinal = root.componentOrdinal;
        table.slotIndex = identity.index;
        table.slotType = identity.type;
        const std::uint32_t tableRow =
            static_cast<std::uint32_t>(context.output->triggerVolumeTables.size());
        context.output->triggerVolumeTables.push_back(table);
        bool tableComplete = true;
        for (std::size_t row = first; row < last; ++row) {
            append_instance(context,
                            tableRow,
                            rows[row].second,
                            root,
                            identity,
                            vertices,
                            triangles,
                            tableComplete);
            if (context.failed) {
                return;
            }
        }
        catalog::TriggerVolumeTable& published = context.output->triggerVolumeTables[tableRow];
        published.instanceCount = static_cast<std::uint32_t>(
            context.output->triggerVolumeInstances.size() - published.firstInstance);
        published.identityMatchCount = static_cast<std::uint32_t>(last - first);
        published.complete = tableComplete;
        if (published.identityMatchCount > 1) {
            ++diagnostics.multipleMatches;
        }
        for (const TriggerVolumeInput& input : inputs) {
            if (cancelled(context)) {
                context.failed = true;
                return;
            }
            append_owner(context, tableRow, input.objectRow, identity);
        }
        first = last;
    }
}

/** Reads one unique config and parses every trigger-volume component it holds. */
void append_config(BuildContext& context,
                   std::span<const TriggerVolumeInput> inputs,
                   std::uint32_t configTag,
                   std::vector<catalog::TriggerVolumeVertex>& vertices,
                   std::vector<catalog::TriggerVolumeTriangle>& triangles) {
    std::uint32_t classId = 0;
    const bool read = package_reader::read_tag(
        *context.source, *context.scratch, configTag, context.configBytes, classId);
    if (read && !collect_inline_name_evidence(*context.output, context.configBytes)) {
        context.failed = true;
        return;
    }
    if (!read || classId != tables::kPlacedConfigClass) {
        mark_unresolved(context);
        return;
    }
    tables::Array components{};
    if (!tables::placed_config_components(context.configBytes, components)
        || components.count > kComponentCapacity) {
        mark_unresolved(context);
        return;
    }
    for (std::size_t componentIndex = 0; componentIndex < components.count; ++componentIndex) {
        if (cancelled(context)) {
            context.failed = true;
            return;
        }
        tables::PlacedConfigComponentRow component{};
        if (!tables::placed_config_component_at(
                context.configBytes, components, componentIndex, component)) {
            mark_unresolved(context);
            return;
        }
        if (component.componentClass == tables::kTriggerVolumeComponentClass) {
            append_root(
                context, inputs, components, componentIndex, configTag, vertices, triangles);
            if (context.failed) {
                return;
            }
        }
    }
}

} // namespace

/** Appends the slot-owned trigger volumes of every reached package config. */
bool append_trigger_volumes(const package_reader::Source& source,
                            package_reader::Scratch& scratch,
                            std::span<const TriggerVolumeInput> inputs,
                            catalog::Snapshot& output,
                            TriggerVolumeCancelCheck cancel) noexcept {
    output.triggerVolumeDiagnostics = {};
    output.triggerVolumeDiagnostics.complete = true;
    if (inputs.empty()) {
        return true;
    }
    try {
        std::vector<TriggerVolumeInput> ordered(inputs.begin(), inputs.end());
        std::sort(ordered.begin(),
                  ordered.end(),
                  [](const TriggerVolumeInput& first, const TriggerVolumeInput& second) noexcept {
                      if (first.configTag != second.configTag) {
                          return first.configTag < second.configTag;
                      }
                      return first.objectRow < second.objectRow;
                  });
        ordered.erase(std::unique(ordered.begin(),
                                  ordered.end(),
                                  [](const TriggerVolumeInput& first,
                                     const TriggerVolumeInput& second) noexcept {
                                      return first.configTag == second.configTag
                                             && first.objectRow == second.objectRow;
                                  }),
                      ordered.end());
        output.triggerVolumeTables.reserve(256);
        output.triggerVolumeOwners.reserve(512);
        output.triggerVolumeInstances.reserve(512);
        output.triggerVolumeVertices.reserve(2'048);
        output.triggerVolumeTriangles.reserve(2'048);
        BuildContext context{&source, &scratch, &output, cancel};
        std::vector<catalog::TriggerVolumeVertex> vertices{};
        std::vector<catalog::TriggerVolumeTriangle> triangles{};
        for (std::size_t first = 0; first < ordered.size();) {
            if (cancelled(context)) {
                return false;
            }
            std::size_t last = first + 1;
            while (last < ordered.size() && ordered[last].configTag == ordered[first].configTag) {
                ++last;
            }
            append_config(context,
                          std::span<const TriggerVolumeInput>(ordered).subspan(first, last - first),
                          ordered[first].configTag,
                          vertices,
                          triangles);
            if (context.failed) {
                return false;
            }
            first = last;
        }
        return !cancelled(context) && append_trigger_volume_incoming_references(output, cancel);
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
