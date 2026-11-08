#include <cstring>

#include "../../../middleware/content/packages/reader/typed_array.h"
#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {
namespace {

/** A selection blob holds one root node; each node lists its child nodes. */
constexpr std::uint64_t kRootNode = offsetof(generated::SReplicationSchema, h9d85f81d);
constexpr std::uint64_t kNodeArrayAt = offsetof(generated::RsatSchemaField, children);
constexpr std::uint64_t kNodeArrayRelativeAt =
    offsetof(generated::RsatSchemaField, children.relative);
constexpr std::uint64_t kNodeSize = sizeof(generated::RsatSchemaField);
constexpr std::uint64_t kNodeBaseAt = offsetof(generated::RsatSchemaField, nodeBase);
constexpr std::uint64_t kNodeSelectedAt = offsetof(generated::RsatSchemaField, rawU32At10);
constexpr std::uint64_t kNodeRepeatAt = offsetof(generated::RsatSchemaField, repeat);
/** A dynamic container places its payload 16 bytes after a 16-aligned header. */
constexpr std::uint64_t kDynamicAlignment = 16;
constexpr std::uint64_t kDynamicHeader = 16;
/** Serialized rows align to the next power of two at or above their size, capped at 16. */
constexpr std::uint64_t kAlignmentCap = 8;

/** Rounds up to a power-of-two alignment; zero or a non power of two is refused. */
[[nodiscard]] bool
align(std::uint64_t value, std::uint64_t alignment, std::uint64_t& output) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    output = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

/** One open scope of the compiled bitmap: schema, its source base, guard bit, first bit. */
struct Frame final {
    std::uint32_t schema{};
    std::uint64_t originalBase{};
    std::uint64_t guard{};
    std::uint64_t first{};
};

/** Compiles one selection tree into serialized-size and bitmap entries. */
class Compiler final {
public:
    Compiler(Store& store, std::span<const std::byte> blob) noexcept : store_(store), blob_(blob) {}

    [[nodiscard]] bool run(std::uint32_t component, Plan& output, const char*& error);

private:
    [[nodiscard]] bool children(std::uint64_t node, std::vector<std::uint64_t>& output);
    [[nodiscard]] bool append(std::uint64_t sourceOffset,
                              std::uint32_t schema,
                              std::uint64_t originalBase,
                              std::int32_t repeat,
                              bool& accepted);
    [[nodiscard]] bool walk_property(const Property& property,
                                     std::uint64_t sourceBase,
                                     std::uint64_t node,
                                     std::uint64_t& cursor,
                                     bool enabled,
                                     std::size_t depth);
    [[nodiscard]] bool walk(std::uint32_t handle,
                            std::uint64_t sourceBase,
                            std::uint64_t node,
                            std::uint64_t& cursor,
                            bool enabled,
                            std::size_t depth);

    Store& store_;
    std::span<const std::byte> blob_;
    std::vector<PlanEntry> entries_{};
    std::vector<Frame> frames_{};
    std::uint64_t serialized_{};
    std::uint64_t bitmap_{};
    std::size_t steps_{};
    const char* error_{};
};

/**
 * Lists the child node offsets of one selection node.
 * @return False when the node or its child array leaves the blob.
 */
bool Compiler::children(std::uint64_t node, std::vector<std::uint64_t>& output) {
    output.clear();
    if (node > blob_.size() || blob_.size() - node < kNodeSize) {
        error_ = "selection_extent";
        return false;
    }
    std::uint64_t raw = 0;
    if (!read_at(blob_, node + kNodeArrayAt, raw)) {
        error_ = "selection_extent";
        return false;
    }
    if (raw == 0) {
        return true;
    }
    std::int64_t relative = 0;
    const std::int64_t header =
        static_cast<std::int64_t>(node + kNodeArrayRelativeAt)
        + (read_at(blob_, node + kNodeArrayRelativeAt, relative) ? relative : 0);
    reader::TypedArrayRows rows{};
    if (header < 0 || raw > kMaximumArrayCount
        || !reader::read_typed_array(blob_,
                                     static_cast<std::size_t>(header),
                                     raw,
                                     kSelectionChildClass,
                                     static_cast<std::size_t>(kNodeSize),
                                     rows)) {
        error_ = "selection_children_extent";
        return false;
    }
    const std::uint64_t start = rows.data;
    output.reserve(static_cast<std::size_t>(raw));
    for (std::uint64_t index = 0; index < raw; ++index) {
        output.push_back(start + kNodeSize * index);
    }
    return true;
}

/**
 * Opens one selected schema at `sourceOffset` and adds its serialized rows and bitmap span.
 * @param accepted Receives whether the schema was entered; a refusal is not a failure.
 * @return False only when metadata is unreadable or a capacity is exceeded.
 */
bool Compiler::append(std::uint64_t sourceOffset,
                      std::uint32_t schema,
                      std::uint64_t originalBase,
                      std::int32_t repeat,
                      bool& accepted) {
    accepted = false;
    if (repeat <= 0) {
        return true;
    }
    const Metadata* selected = store_.get(schema, error_);
    if (selected == nullptr) {
        return false;
    }
    if (!selected->codec.present || selected->codec.fields.empty()) {
        return true;
    }
    while (frames_.size() > 1 && frames_.back().originalBase > sourceOffset) {
        frames_.pop_back();
    }
    const Frame parent = frames_.back();
    const Metadata* parentRecord = store_.get(parent.schema, error_);
    if (parentRecord == nullptr) {
        return false;
    }
    const Codec& parentCodec = parentRecord->codec;
    if (!parentCodec.present || parentCodec.original == 0) {
        return true;
    }
    const std::uint64_t occurrence = (sourceOffset - parent.originalBase) / parentCodec.original;
    const std::uint64_t offset = (sourceOffset - parent.originalBase) % parentCodec.original;
    std::int64_t guard = 0;
    if (!property_guard(store_, parent.schema, offset, guard, error_)) {
        return false;
    }
    if (guard < 0) {
        return true;
    }
    const std::uint64_t guardBit = guard == 0
                                       ? parent.guard
                                       : static_cast<std::uint64_t>(guard) + parent.first
                                             + std::uint64_t{parentCodec.flags} * occurrence - 1;
    frames_.push_back({schema, originalBase, guardBit, bitmap_});
    if (frames_.size() > kMaximumDepth) {
        error_ = "compiled_stack_capacity";
        return false;
    }
    const std::uint64_t size = selected->codec.serialized;
    if (size != 0) {
        if (static_cast<std::uint32_t>(repeat) > kMaximumRepeat) {
            error_ = "selection_repeat_capacity";
            return false;
        }
        std::uint64_t mask = 0;
        for (std::uint64_t bit = 1; bit <= kAlignmentCap; bit <<= 1U) {
            mask += size > bit ? bit : 0;
        }
        std::uint64_t aligned = 0;
        std::uint64_t rowAligned = 0;
        if (!align(serialized_, mask + 1, aligned) || !align(size, mask + 1, rowAligned)) {
            error_ = "alignment";
            return false;
        }
        serialized_ = aligned + size;
        if (repeat > 1) {
            serialized_ += rowAligned * (static_cast<std::uint64_t>(repeat) - 1);
        }
        const std::uint32_t flags = selected->codec.flags;
        entries_.push_back({schema,
                            static_cast<std::uint32_t>(repeat),
                            static_cast<std::uint32_t>(guardBit),
                            static_cast<std::uint32_t>(bitmap_),
                            repeat > 1 ? flags : 0U});
        bitmap_ += std::uint64_t{flags} * static_cast<std::uint64_t>(repeat);
    }
    accepted = true;
    return true;
}

/**
 * Walks one declared property node and advances the serialized cursor past its payload.
 * @return False when metadata, alignment or the blob refuses the node.
 */
bool Compiler::walk_property(const Property& property,
                             std::uint64_t sourceBase,
                             std::uint64_t node,
                             std::uint64_t& cursor,
                             bool enabled,
                             std::size_t depth) {
    const std::uint32_t declared = property.memberType;
    const std::uint64_t source = sourceBase + property.byteOffset;
    const Metadata* native = store_.get(declared, error_);
    if (native == nullptr) {
        return false;
    }
    std::uint32_t selected = 0;
    std::int32_t repeat = 0;
    std::vector<std::uint64_t> nodes;
    if (!read_at(blob_, node + kNodeSelectedAt, selected)
        || !read_at(blob_, node + kNodeRepeatAt, repeat)) {
        error_ = "selection_extent";
        return false;
    }
    if (!children(node, nodes)) {
        return false;
    }
    const bool container = native->kind == kKindContainer;
    const std::uint32_t generic = native->hasGeneric ? native->generic : 0;
    const bool generics = native->hasGeneric;
    const bool simple =
        native->kind == kKindValue || (container && generics && generic == kGenericSimple);
    const bool dynamic =
        !simple && container
        && ((generics && (generic == kGenericDynamicA || generic == kGenericDynamicB))
            || declared == kDynamicDeclared);
    const bool inlineChildren = !simple && !dynamic && container && generics
                                && (generic == kGenericInline || generic == kGenericCounted);
    if (inlineChildren) {
        if (nodes.empty()) {
            return true;
        }
        const Metadata* child = store_.get(selected, error_);
        if (child == nullptr) {
            return false;
        }
        std::uint64_t start = source;
        if (generic != kGenericInline && !align(source + 4, child->alignment, start)) {
            error_ = "alignment";
            return false;
        }
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            if (!walk(selected,
                      start + std::uint64_t{child->size} * index,
                      nodes[index],
                      cursor,
                      enabled,
                      depth + 1)) {
                return false;
            }
        }
        return true;
    }
    if (!simple && !dynamic) {
        return !container || walk(declared, source, node, cursor, enabled, depth + 1);
    }
    std::uint64_t start = cursor;
    const Metadata* child = nullptr;
    if (repeat > 0) {
        child = store_.get(selected, error_);
        if (child == nullptr) {
            return false;
        }
        if (simple) {
            if (!align(cursor + 4, child->alignment, start)) {
                error_ = "alignment";
                return false;
            }
        } else {
            if (!align(cursor + 4, kDynamicAlignment, start)) {
                error_ = "alignment";
                return false;
            }
            start += kDynamicHeader;
        }
        cursor = start + std::uint64_t{child->size} * static_cast<std::uint64_t>(repeat);
    }
    bool accepted = false;
    if (enabled && !append(source, selected, start, repeat, accepted)) {
        return false;
    }
    if (child != nullptr && child->hasProperties) {
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            if (!walk(selected,
                      start + std::uint64_t{child->size} * index,
                      nodes[index],
                      cursor,
                      accepted,
                      depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Walks every property the schema declares that the selection node names.
 * @return False when a capacity is exceeded or a nested walk fails.
 */
bool Compiler::walk(std::uint32_t handle,
                    std::uint64_t sourceBase,
                    std::uint64_t node,
                    std::uint64_t& cursor,
                    bool enabled,
                    std::size_t depth) {
    ++steps_;
    if (depth > kMaximumDepth || steps_ > kMaximumSteps) {
        error_ = "selection_traversal_capacity";
        return false;
    }
    const Metadata* native = store_.get(handle, error_);
    if (native == nullptr) {
        return false;
    }
    if (!native->hasProperties) {
        return true;
    }
    std::vector<std::uint64_t> nodes;
    if (!children(node, nodes)) {
        return false;
    }
    std::int64_t relative = 0;
    if (!read_at(blob_, node + kNodeBaseAt, relative)) {
        error_ = "selection_extent";
        return false;
    }
    const Property base = native->base;
    if (base.memberType != 0 && base.memberType != kAbsent && relative != 0) {
        const std::int64_t target = static_cast<std::int64_t>(node + kNodeBaseAt) + relative;
        if (target < 0) {
            error_ = "selection_extent";
            return false;
        }
        if (!walk_property(
                base, sourceBase, static_cast<std::uint64_t>(target), cursor, enabled, depth)) {
            return false;
        }
    }
    const std::vector<Property> properties = native->properties;
    for (const Property& property : properties) {
        for (const std::uint64_t child : nodes) {
            std::uint32_t nameHash = 0;
            if (!read_at(blob_, child, nameHash)) {
                error_ = "selection_extent";
                return false;
            }
            if (nameHash != property.nameHash) {
                continue;
            }
            if (!walk_property(property, sourceBase, child, cursor, enabled, depth)) {
                return false;
            }
            break;
        }
    }
    return true;
}

/**
 * Compiles the whole tree for one component.
 * @return False with `error` naming the refusal; the plan is unchanged then.
 */
bool Compiler::run(std::uint32_t component, Plan& output, const char*& error) {
    const Metadata* root = store_.get(component, error_);
    if (root == nullptr) {
        error = error_;
        return false;
    }
    if (!root->codec.present) {
        error = "component_codec_absent";
        return false;
    }
    serialized_ = root->codec.serialized;
    bitmap_ = std::uint64_t{root->codec.flags} + 1;
    entries_.assign(1, PlanEntry{component, 1, 0, 1, 0});
    frames_.assign(1, Frame{component, 0, 0, 1});
    steps_ = 0;
    std::uint64_t cursor = root->size;
    if (!walk(component, 0, kRootNode, cursor, true, 0)) {
        error = error_;
        return false;
    }
    if (bitmap_ > 0xFFFFFFFFULL) {
        error = "bitmap_capacity";
        return false;
    }
    output.component = component;
    if (serialized_ == 0) {
        output.entries.clear();
        output.bitmapBits = 0;
        output.active = false;
        return true;
    }
    output.entries = std::move(entries_);
    output.bitmapBits = static_cast<std::uint32_t>(bitmap_);
    output.active = true;
    return true;
}

} // namespace

/**
 * Compiles one selection blob against its component's reflection metadata.
 * @param store Metadata cache shared by every pair of the build.
 * @param component Component definition handle.
 * @param blob Selection package entry.
 * @param output Receives the plan on success.
 * @param error Receives the refusal name on failure.
 * @return False when the pair is refused; the caller counts it and moves on.
 */
bool compile(Store& store,
             std::uint32_t component,
             std::span<const std::byte> blob,
             Plan& output,
             const char*& error) noexcept {
    try {
        Compiler compiler(store, blob);
        return compiler.run(component, output, error);
    } catch (...) {
        error = "allocation";
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal
