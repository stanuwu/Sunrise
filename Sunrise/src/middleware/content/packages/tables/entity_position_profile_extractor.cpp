#include "entity_position_profile_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "../reader/layout.h"
#include "../schema/generated/tag_classes_80807d.h"
#include "../schema/generated/tag_classes_808091.h"
#include "../schema/generated/tag_classes_808092.h"
#include "../schema/generated/tag_classes_808096.h"
#include "../schema/generated/tag_classes_808098.h"
#include "../schema/generated/tag_classes_808099.h"
#include "../schema/generated/tag_classes_80809f.h"

namespace sunrise::middleware::content::packages::position_profiles {
namespace {
namespace generated = schema::generated;
// Classes of the map tree the walk follows, then the all-ones absent marker.
constexpr std::uint32_t kMapRoot = generated::SMapRoot::kClass;
constexpr std::uint32_t kMap = generated::SMapLocationNames::kClass;
constexpr std::uint32_t kBubble = generated::SBubbleParent::kClass;
constexpr std::uint32_t kBounds = generated::BubbleBounds::kClass;
constexpr std::uint32_t kScenario = generated::SScenario::kClass;
constexpr std::uint32_t kAbsent = 0xFFFFFFFF;
using ArrayHeader = reader::layout::TypedArrayHeader;
/** A reference holds its wide key at the same offset in every referencing row. */
constexpr std::size_t kWideKey = offsetof(generated::MapBubbleRef, wideTag);
static_assert(kWideKey == offsetof(generated::BubbleBoundsRef, wideTag));
using Blob = std::vector<std::byte>;
using Rows = state::gameplay::entity_position_profiles::Rows;
template <class T> T value(std::span<const std::byte> bytes, std::size_t offset) {
    T result{};
    if (offset > bytes.size() || sizeof result > bytes.size() - offset) {
        throw 0;
    }
    std::memcpy(&result, bytes.data() + offset, sizeof result);
    return result;
}
struct Map final {
    std::vector<std::uint32_t> hashes, owners;
    std::vector<std::pair<std::uint16_t, std::array<std::uint8_t, 3>>> cells;
    bool base{};
};
/** One resolved bubble width and the handle it came from. */
struct BoundBits final {
    std::uint32_t handle{};
    std::array<std::uint8_t, 3> bits{};
};
struct Extractor final {
    std::span<const KeyTag> keys;
    Read read;
    void* context;
    /** Resolved widths, ordered by handle so a repeat costs a search, not a read. */
    std::vector<BoundBits> bounds;
    Blob tag(std::uint32_t handle, std::uint32_t cls) {
        Blob bytes;
        if (!read(context, handle, cls, bytes)) {
            throw 0;
        }
        return bytes;
    }
    /** Unresolved keys cannot substitute for a validated live tag. */
    std::pair<std::uint32_t, Blob>
    reference(std::span<const std::byte> bytes, std::size_t offset, std::uint32_t cls) {
        auto handle = value<std::uint32_t>(bytes, offset);
        if (handle == kAbsent) {
            const auto key = value<std::uint64_t>(bytes, offset + kWideKey);
            const auto found =
                std::lower_bound(keys.begin(), keys.end(), key, [](const KeyTag& row, auto target) {
                    return row.key < target;
                });
            if (found == keys.end() || found->key != key || found->classId != cls) {
                throw 0;
            }
            handle = found->tag;
        }
        return {handle, tag(handle, cls)};
    }
    std::vector<std::size_t> members(std::span<const std::byte> bytes,
                                     std::size_t field,
                                     std::size_t stride,
                                     std::uint32_t cls) {
        std::vector<std::size_t> offsets;
        if (!array(bytes, field, stride, cls, offsets)) {
            throw 0;
        }
        return offsets;
    }
    /** Row offsets of one typed array field; its stride and element class come from `Row`. */
    template <class Row>
    std::vector<std::size_t> members(std::span<const std::byte> bytes, std::size_t field) {
        return members(bytes, field, sizeof(Row), Row::kClass);
    }
    /** Every referenced bound must resolve before its union can be used. */
    std::array<std::uint8_t, 3> bubble(std::span<const std::byte> root, std::size_t offset) {
        auto [handle, bytes] = reference(root, offset, kBubble);
        const auto ordered = [](const BoundBits& row, std::uint32_t target) noexcept {
            return row.handle < target;
        };
        if (const auto cached = std::lower_bound(bounds.begin(), bounds.end(), handle, ordered);
            cached != bounds.end() && cached->handle == handle) {
            return cached->bits;
        }
        // Authored empty bounds are stored as this exact sentinel pair and are skipped.
        constexpr float maximum = (std::numeric_limits<float>::max)();
        std::array<float, 3> low{maximum, maximum, maximum}, high{-maximum, -maximum, -maximum};
        bool any = false;
        for (auto member : members<generated::BubbleBoundsRef>(
                 bytes, offsetof(generated::SBubbleParent, metadata.boundsRefs))) {
            const auto [boundTag, bound] = reference(bytes, member, kBounds);
            (void)boundTag;
            if (bound.size() != sizeof(generated::BubbleBounds)) {
                throw 0;
            }
            std::array<float, 3> a{}, b{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                a[axis] = value<float>(
                    bound, offsetof(generated::BubbleBounds, min) + axis * sizeof(float));
                b[axis] = value<float>(
                    bound, offsetof(generated::BubbleBounds, max) + axis * sizeof(float));
            }
            if (a == std::array<float, 3>{maximum, maximum, maximum}
                && b == std::array<float, 3>{-maximum, -maximum, -maximum}) {
                continue;
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(a[axis]) || !std::isfinite(b[axis]) || a[axis] > b[axis]) {
                    throw 0;
                }
                low[axis] = (std::min)(low[axis], a[axis]);
                high[axis] = (std::max)(high[axis], b[axis]);
            }
            any = true;
        }
        if (!any) {
            throw 0;
        }
        std::array<std::uint8_t, 3> bits{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float extent = high[axis] - low[axis];
            const float scaled = extent * 20.0F;
            const double steps = std::ceil(static_cast<double>(scaled));
            if (!std::isfinite(steps) || steps < 0 || steps > 2147483648.0) {
                throw 0;
            }
            auto remaining = steps <= 1 ? 0U : static_cast<std::uint32_t>(steps - 1);
            while (remaining) {
                ++bits[axis];
                remaining >>= 1;
            }
        }
        bounds.insert(std::lower_bound(bounds.begin(), bounds.end(), handle, ordered),
                      BoundBits{handle, bits});
        return bits;
    }
    /** Cell ownership must agree with the map's ordered bubble table. */
    Map map(const NamedTag& name) {
        const auto root = tag(name.tag, kMapRoot);
        const auto definition =
            tag(value<std::uint32_t>(root, offsetof(generated::SMapRoot, mapDefinition)), kMap);
        const auto refs =
            members<generated::MapBubbleRef>(root, offsetof(generated::SMapRoot, mapEntries));
        const auto cells =
            members<generated::MapCell>(definition, offsetof(generated::SMapLocationNames, slices));
        const auto bubbles = members<generated::MapBubble>(
            definition, offsetof(generated::SMapLocationNames, sliceSets));
        if (refs.empty() || refs.size() != bubbles.size()) {
            throw 0;
        }
        Map result;
        result.base = name.basePackage;
        for (std::size_t index = 0; index < bubbles.size(); ++index) {
            if (value<std::uint32_t>(definition,
                                     bubbles[index] + offsetof(generated::MapBubble, bubbleOrdinal))
                != index) {
                throw 0;
            }
            result.hashes.push_back(value<std::uint32_t>(
                definition, bubbles[index] + offsetof(generated::MapBubble, bubbleNameHash)));
        }
        for (std::size_t index = 0; index < cells.size(); ++index) {
            auto owner = value<std::uint32_t>(
                definition, cells[index] + offsetof(generated::MapCell, ownerBubble));
            if (owner == kAbsent && index < 256) {
                for (std::size_t bubbleIndex = 0; bubbleIndex < bubbles.size(); ++bubbleIndex) {
                    if ((value<std::uint8_t>(definition,
                                             bubbles[bubbleIndex]
                                                 + offsetof(generated::MapBubble, cellMask)
                                                 + index / 8)
                         & (1U << (index % 8)))
                        != 0) {
                        owner = static_cast<std::uint32_t>(bubbleIndex);
                        break;
                    }
                }
            }
            result.owners.push_back(owner);
            if (index > 255 || owner == kAbsent) {
                continue;
            }
            if (owner >= refs.size()) {
                throw 0;
            }
            try {
                result.cells.emplace_back(static_cast<std::uint16_t>(index),
                                          bubble(root, refs[owner]));
            } catch (...) {}
        }
        return result;
    }
    /** Ambiguous map signatures leave the entire scenario without guessed profiles. */
    bool scenario(const NamedTag& name, const std::vector<Map>& maps, Rows& rows) {
        const auto bytes = tag(name.tag, kScenario);
        const auto bubbles =
            members<generated::ScenarioBubble>(bytes, offsetof(generated::SScenario, bubbles));
        std::vector<std::uint32_t> hashes;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> owners;
        for (std::size_t index = 0; index < bubbles.size(); ++index) {
            hashes.push_back(value<std::uint32_t>(
                bytes, bubbles[index] + offsetof(generated::ScenarioBubble, nameHash)));
            for (auto state : members<generated::SliceSetState>(
                     bytes, bubbles[index] + offsetof(generated::ScenarioBubble, states))) {
                owners.emplace_back(
                    value<std::uint32_t>(
                        bytes, state + offsetof(generated::SliceSetState, mapBubbleIndex)),
                    static_cast<std::uint32_t>(index));
            }
        }
        if (owners.empty()) {
            return false;
        }
        std::vector<const Map*> matches;
        for (const auto& candidate : maps) {
            if (candidate.hashes == hashes
                && std::all_of(owners.begin(), owners.end(), [&](auto owner) {
                       return owner.first < candidate.owners.size()
                              && candidate.owners[owner.first] == owner.second;
                   })) {
                matches.push_back(&candidate);
            }
        }
        if (std::any_of(
                matches.begin(), matches.end(), [](auto candidate) { return candidate->base; })) {
            std::erase_if(matches, [](auto candidate) { return !candidate->base; });
        }
        if (matches.empty()) {
            return false;
        }
        for (auto candidate : matches) {
            if (candidate->cells != matches.front()->cells) {
                return false;
            }
        }
        // Only the client scenario row names an activity; the suffix is stripped to get it.
        constexpr std::string_view suffix = ":scenario_client";
        const std::string_view text = name.text();
        if (!text.ends_with(suffix)) {
            return false;
        }
        const auto activity = text.substr(0, text.size() - suffix.size());
        for (const auto& cell : matches.front()->cells) {
            rows.push_back({activity,
                            cell.first,
                            cell.second,
                            static_cast<std::uint8_t>(matches.front()->owners[cell.first])});
        }
        return true;
    }
};
} // namespace
NamedTag::NamedTag(std::string_view text,
                   std::uint32_t tagValue,
                   std::uint32_t classValue,
                   bool base) noexcept
    : tag(tagValue), classId(classValue), basePackage(base) {
    if (text.size() <= name.size()) {
        std::copy(text.begin(), text.end(), name.begin());
        nameLength = text.size();
    }
}
/** @return The stored name, empty when none fit. */
std::string_view NamedTag::text() const noexcept {
    return {name.data(), nameLength};
}
/** Array headers carry their marker four bytes before the relative target. */
bool array(std::span<const std::byte> bytes,
           std::size_t field,
           std::size_t stride,
           std::uint32_t elementClass,
           std::vector<std::size_t>& offsets) noexcept {
    offsets.clear();
    try {
        const auto count = value<std::uint64_t>(bytes, field);
        if (count == 0) {
            return true;
        }
        const auto relative =
            value<std::int64_t>(bytes, field + reader::layout::kArrayFieldRelative);
        if (relative < 0 || static_cast<std::uint64_t>(relative) > bytes.size()
            || count > 1048576) {
            return false;
        }
        const auto header =
            field + reader::layout::kArrayFieldRelative + static_cast<std::size_t>(relative);
        // Unlike read_typed_array, the header's count is not checked and a zero stride fails.
        if (header < ArrayHeader::kMarkerBack || header > bytes.size()
            || bytes.size() - header < ArrayHeader::kSize
            || value<std::uint32_t>(bytes, header - ArrayHeader::kMarkerBack)
                   != reader::layout::kTableArrayMarker
            || value<std::uint32_t>(bytes, header + ArrayHeader::kClass) != elementClass
            || stride == 0 || count > (bytes.size() - header - ArrayHeader::kSize) / stride) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            offsets.push_back(header + ArrayHeader::kSize + index * stride);
        }
        return true;
    } catch (...) {
        offsets.clear();
        return false;
    }
}
/** Only complete map/scenario joins publish widths; unresolved cells remain absent. */
bool extract(std::span<const NamedTag> names,
             std::span<const KeyTag> keys,
             Read read,
             void* context,
             Rows& rows) noexcept {
    rows.clear();
    if (!read) {
        return false;
    }
    try {
        Extractor extractor{keys, read, context, {}};
        std::vector<Map> maps;
        for (const auto& name : names) {
            if (name.classId == kMapRoot) {
                try {
                    maps.push_back(extractor.map(name));
                } catch (...) {}
            }
        }
        for (const auto& name : names) {
            if (name.classId == kScenario) {
                try {
                    (void)extractor.scenario(name, maps, rows);
                } catch (...) {}
            }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.activity < b.activity || (a.activity == b.activity && a.cell < b.cell);
        });
        return state::gameplay::entity_position_profiles::validate(rows);
    } catch (...) {
        rows.clear();
        return false;
    }
}
} // namespace sunrise::middleware::content::packages::position_profiles
