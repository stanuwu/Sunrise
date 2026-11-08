#include "studio_pose_capture.h"

#include <algorithm>
#include <limits>
#include <span>

#include "../../../middleware/content/packages/schema/generated/tag_layout_808073_0.h"

namespace sunrise::client::diagnostics::studio {
namespace {

/** Native draw records retain the component, owner, model, consumed bone count and palette form. */
struct NativeDraw final {
    std::uint32_t component{}, owner{}, model{};
    std::array<std::byte, 100> reserved{};
    std::uint32_t bones{}, kind{};
};

/** The producer owns these pointers until its caller returns to the frame allocator. */
struct NativePose final {
    std::array<std::uintptr_t, 3> palettes{};
    std::uint32_t bones{}, flags{};
    std::uintptr_t transforms{};
    std::array<std::byte, 56> reserved{};
    pose_wire::Vector lighting{};
};

/** Offsets belong to the native draw and evaluated-pose records. */
constexpr std::size_t kDrawBones = 112, kPoseLighting = 96;
static_assert(offsetof(NativeDraw, bones) == kDrawBones);
static_assert(offsetof(NativePose, lighting) == kPoseLighting);
using middleware::content::packages::schema::generated::SEntityModel;
using pose_wire::kNoOwner;
/** A model row is one vector: the offset and scale W, then the UV scale and offset. */
static_assert(offsetof(SEntityModel, uvScaleU) - offsetof(SEntityModel, positionOffset)
              == sizeof(pose_wire::Vector));

/** Runtime table rows link components at byte 20 and store their body displacement at byte 8. */
struct ComponentEntry final {
    std::uint64_t prefix{}, correction{};
    std::uint32_t opaque16{}, next{};
};
struct RuntimeTable final {
    std::uintptr_t unused{}, rows{}, pool{};
    std::array<std::byte, 24> reserved{};
    std::uint32_t stride{};
    std::int32_t correctionMask{};
    std::uint64_t tail{};
};
struct GenerationPool final {
    std::uint64_t prefix{};
    std::uintptr_t data{};
    std::array<std::byte, 12> reserved{};
    std::uint32_t generationOffset{}, stride{};
};
struct ObjectPrefix final {
    std::array<std::byte, 12> prefix{};
    std::uint32_t self{};
    std::array<std::byte, 60> reserved{};
    std::uint32_t components{};
};
struct ComponentPrefix final {
    std::array<std::byte, 36> prefix{};
    std::uint32_t self{}, reserved{}, owner{};
};
/** These byte offsets belong to the native pool, object and component layouts. */
constexpr std::size_t kRuntimeTableBytes = 64, kTablePoolOffset = 16, kTableStrideOffset = 48,
                      kPoolGenerationOffset = 28, kObjectSelfOffset = 12,
                      kObjectComponentsOffset = 76, kComponentOwnerOffset = 44;
static_assert(sizeof(RuntimeTable) == kRuntimeTableBytes);
static_assert(offsetof(RuntimeTable, pool) == kTablePoolOffset);
static_assert(offsetof(RuntimeTable, stride) == kTableStrideOffset);
static_assert(offsetof(GenerationPool, generationOffset) == kPoolGenerationOffset);
static_assert(offsetof(ObjectPrefix, self) == kObjectSelfOffset);
static_assert(offsetof(ObjectPrefix, components) == kObjectComponentsOffset);
static_assert(offsetof(ComponentPrefix, owner) == kComponentOwnerOffset);
/** Component walks are diagnostic work and never follow more than one slot pool. */
constexpr std::size_t kMaximumComponents = pose_wire::kSlotMask + 1;
/** Handle table selection uses ten bits, extended to fourteen by the sign bit. */
constexpr std::uint32_t kHandleIndexBits = 13, kHandleSignBit = 0x80000000U,
                        kRuntimeTableMask = 0x3FF, kExtendedTableMask = 0x3FFF;

template <typename Value>
[[nodiscard]] bool
read(const content::handles::Source& source, std::uintptr_t address, Value& output) noexcept {
    return address != 0 && source.read != nullptr
           && source.read(source.context, address, std::as_writable_bytes(std::span(&output, 1)));
}

/** The runtime table selector keeps ten bits, or fourteen for handles with their top bit set. */
[[nodiscard]] bool runtime_entry(const content::handles::Source& source,
                                 std::uint32_t handle,
                                 RuntimeTable& table,
                                 std::uintptr_t& entry) noexcept {
    std::uintptr_t object = 0, tables = 0;
    if (handle == kNoOwner || !read(source, source.tablesSlot, object)
        || !read(source, object, tables) || tables == 0) {
        return false;
    }
    const auto index = (handle >> kHandleIndexBits)
                       & ((handle & kHandleSignBit) != 0 ? kExtendedTableMask : kRuntimeTableMask);
    if (tables > (std::numeric_limits<std::uintptr_t>::max)() - sizeof(RuntimeTable) * index
        || !read(source, tables + sizeof(RuntimeTable) * index, table) || table.rows == 0
        || table.stride == 0) {
        return false;
    }
    const auto offset = std::uint64_t(table.stride) * (handle & pose_wire::kSlotMask);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()
        || table.rows > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return false;
    }
    entry = table.rows + static_cast<std::uintptr_t>(offset);
    return true;
}

/** The generation comes from the same pool lane read by the native handle-pair getter. */
[[nodiscard]] bool generation(const content::handles::Source& source,
                              std::uint32_t handle,
                              std::uint32_t& output) noexcept {
    RuntimeTable table{};
    GenerationPool pool{};
    std::uintptr_t entry = 0;
    if (!runtime_entry(source, handle, table, entry) || !read(source, table.pool, pool)
        || pool.data == 0 || pool.stride < sizeof(output)) {
        return false;
    }
    const auto offset =
        std::uint64_t(pool.stride) * (handle & pose_wire::kSlotMask) + pool.generationOffset;
    if (pool.data > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return false;
    }
    return read(source, pool.data + static_cast<std::uintptr_t>(offset), output)
           && output != kNoOwner;
}

} // namespace

/** Recheck both words so an allocation change during the read cannot supply a mixed identity. */
bool describe_pose_owner(const content::handles::Source& source,
                         PoseObjectPool pool,
                         std::uint32_t handle,
                         PoseOwner& output) noexcept {
    std::uintptr_t base = 0;
    std::uint32_t stride = 0, before = 0, after = 0;
    if (handle == kNoOwner || !read(source, pool.baseSlot, base)
        || !read(source, pool.strideSlot, stride) || base == 0 || stride < sizeof(ObjectPrefix)
        || !generation(source, handle, before)) {
        return false;
    }
    const auto offset = std::uint64_t(stride) * (handle & pose_wire::kSlotMask);
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return false;
    }
    ObjectPrefix first{}, second{};
    if (!read(source, base + static_cast<std::uintptr_t>(offset), first) || first.self != handle
        || !generation(source, handle, after) || before != after
        || !read(source, base + static_cast<std::uintptr_t>(offset), second)
        || second.self != handle) {
        return false;
    }
    output = {handle, before};
    return true;
}

/** The owner's current chain must contain the exact component handle, not just its pool slot. */
bool describe_pose_identity(const content::handles::Source& source,
                            PoseObjectPool pool,
                            std::uintptr_t draw,
                            PoseIdentity& output) noexcept {
    NativeDraw header{};
    PoseIdentity identity{};
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!read(source, draw, header) || header.owner == kNoOwner
        || !read(source, pool.baseSlot, base) || !read(source, pool.strideSlot, stride) || base == 0
        || stride < sizeof(ObjectPrefix)) {
        return false;
    }
    const auto offset = std::uint64_t(stride) * (header.owner & pose_wire::kSlotMask);
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return false;
    }
    ObjectPrefix object{};
    if (!read(source, base + static_cast<std::uintptr_t>(offset), object)
        || object.self != header.owner || object.components == kNoOwner
        || !generation(source, header.owner, identity.ownerGeneration)
        || !generation(source, header.component, identity.componentGeneration)) {
        return false;
    }
    RuntimeTable table{};
    ComponentEntry node{};
    std::uintptr_t entry = 0;
    std::uint32_t cursor = object.components;
    for (std::size_t count = 0; count < kMaximumComponents && cursor != kNoOwner; ++count) {
        if (!runtime_entry(source, cursor, table, entry) || table.stride < sizeof(ComponentEntry)
            || !read(source, entry, node)) {
            return false;
        }
        if (cursor == header.component) {
            const auto mask =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(table.correctionMask));
            ComponentPrefix component{};
            if (!read(
                    source, entry - static_cast<std::uintptr_t>(node.correction & mask), component)
                || component.self != header.component || component.owner != header.owner) {
                return false;
            }
            identity.owner = header.owner;
            identity.component = header.component;
            output = identity;
            return true;
        }
        if (node.next == cursor) {
            return false;
        }
        cursor = node.next;
    }
    return false;
}

/** Palette forms 1, 2 and 3 store 2, 3 and 4 vectors per bone in pose slots 2, 0 and 1. */
bool describe_draw(const content::handles::Source& source,
                   std::uintptr_t draw,
                   std::uintptr_t pose,
                   pose_wire::Draw& output,
                   std::uintptr_t& palette) noexcept {
    NativeDraw header{};
    NativePose frame{};
    if (!read(source, draw, header) || header.owner == kNoOwner || header.kind < 1
        || header.kind > 3 || header.bones == 0 || header.bones > pose_wire::kBoneCapacity
        || !read(source, pose, frame) || frame.bones < header.bones
        || frame.bones > pose_wire::kBoneCapacity) {
        return false;
    }
    const std::size_t slot = header.kind == 1 ? 2 : header.kind == 2 ? 0 : 1;
    std::uintptr_t model = 0;
    pose_wire::Draw row{};
    if (frame.palettes[slot] == 0 || !content::handles::resolve(source, header.model, model)
        || !read(source, model + offsetof(SEntityModel, positionOffset), row.fixedRows[0])
        || !read(source, model + offsetof(SEntityModel, uvScaleU), row.fixedRows[1])) {
        return false;
    }
    row.fixedRows[2] = frame.lighting;
    row.owner = header.owner;
    row.component = header.component;
    row.model = header.model;
    row.kind = header.kind;
    row.modelBones = header.bones;
    row.poseBones = frame.bones;
    row.count = header.bones * (header.kind + 1);
    output = row;
    palette = frame.palettes[slot];
    return true;
}

/** Reused slots and generations cannot supply models to a different object query. */
std::size_t copy_pose_models(std::span<const pose_wire::Draw> draws,
                             PoseOwner owner,
                             std::uint64_t captureTime,
                             std::span<std::uint32_t> models,
                             std::uint64_t& time) noexcept {
    time = 0;
    std::array<std::uint32_t, pose_wire::kDrawCapacity> seen{};
    std::size_t count = 0;
    for (const auto& draw : draws.first(std::min(draws.size(), seen.size()))) {
        if (draw.owner != owner.handle || draw.ownerGeneration != owner.generation
            || draw.model == 0) {
            continue;
        }
        const auto known = std::span(seen).first(count);
        if (std::find(known.begin(), known.end(), draw.model) != known.end()) {
            continue;
        }
        if (count < models.size()) {
            models[count] = draw.model;
        }
        seen[count++] = draw.model;
    }
    if (count != 0) {
        time = captureTime;
    }
    return count;
}

} // namespace sunrise::client::diagnostics::studio
