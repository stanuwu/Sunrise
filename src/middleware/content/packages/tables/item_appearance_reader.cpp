#include <cstring>

#include "definition_index_table.h"
#include "items.h"

namespace sunrise::middleware::content::packages::tables::items {
namespace {

/** The art block declares its gear art definition index here. */
constexpr std::size_t kGearArtIndexOffset = 88;
/** One art row is 4 bytes and carries its art index after a class byte and one pad byte. */
constexpr std::size_t kArtRowValueOffset = 2;
/** Material override arrays sit at these art-block offsets, one per stage. */
constexpr std::size_t kMaterialStageOffsets[]{40, 56, 72};
/** One material override row is 4 bytes: a signed key, one pad byte, then a value. */
constexpr std::size_t kMaterialRowStride = 4;
/** A material override row carries its value after its key and one pad byte. */
constexpr std::size_t kMaterialRowValueOffset = 2;
/** The stat block holds the sandbox perk array descriptor at this offset inside itself. */
constexpr std::size_t kSandboxPerkDescriptor = 16;
/** One sandbox perk entry is 24 bytes and begins with its perk index. */
constexpr std::size_t kSandboxPerkStride = 24;
/** All bits set marks a declared-empty sandbox perk entry. */
constexpr std::uint16_t kUnavailableSandboxPerk = 0xFFFF;

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/**
 * Turns a self-relative block offset a definition declares into a blob offset.
 * @param definition Whole item definition bytes.
 * @param member Offset of the self-relative field.
 * @param block Receives the block offset when the field names one.
 * @return True when the field is present, nonzero and lands inside the blob.
 */
[[nodiscard]] bool resolve_block(std::span<const std::byte> definition,
                                 std::size_t member,
                                 std::size_t& block) noexcept {
    std::int64_t relative = 0;
    if (!read(definition, member, relative) || relative == 0) {
        return false;
    }
    const std::int64_t resolved = static_cast<std::int64_t>(member) + relative;
    if (resolved < 0 || static_cast<std::uint64_t>(resolved) >= definition.size()) {
        return false;
    }
    block = static_cast<std::size_t>(resolved);
    return true;
}

/**
 * Reads the two art indices the art block declares.
 * The arrangement index comes from the block's first art row, so the row count must be checked.
 * An art block with no rows would read arrangement zero, which is a real arrangement, not none.
 * @param definition Whole item definition bytes.
 * @param art Art block offset.
 * @param row Receives both art indices.
 */
void read_art(std::span<const std::byte> definition, std::size_t art, Row& row) noexcept {
    (void)read(definition, art + kGearArtIndexOffset, row.gearArtIndex);
    Array rows{};
    if (!find_array_at(definition, art, rows) || rows.elementClass != kArtRowClass) {
        return;
    }
    (void)read(definition, rows.dataOffset + kArtRowValueOffset, row.artArrangementIndex);
}

/**
 * Reads every material override row the art block's three stage arrays declare.
 * Stage order is kept because the consumer folds base and plug rows stage by stage. A row whose
 * key is empty is not a row.
 * @param definition Whole item definition bytes.
 * @param art Art block offset.
 * @param row Receives the override rows in stage order.
 */
void read_material_overrides(std::span<const std::byte> definition,
                             std::size_t art,
                             Row& row) noexcept {
    for (std::size_t stage = 0; stage < std::size(kMaterialStageOffsets); ++stage) {
        Array overrides{};
        if (!find_array_at(definition, art + kMaterialStageOffsets[stage], overrides)
            || overrides.elementClass != kMaterialOverrideClass) {
            continue;
        }
        for (std::uint64_t entry = 0; entry < overrides.count; ++entry) {
            if (row.renderOverrideCount >= kRenderOverrideCapacity) {
                return;
            }
            const std::size_t at =
                overrides.dataOffset + static_cast<std::size_t>(entry) * kMaterialRowStride;
            std::int8_t key = kEmptyOverrideKey;
            std::uint16_t value = 0;
            if (!read(definition, at, key)
                || !read(definition, at + kMaterialRowValueOffset, value)) {
                return;
            }
            if (key == kEmptyOverrideKey) {
                continue;
            }
            row.renderOverrides[row.renderOverrideCount++] = {
                static_cast<std::uint8_t>(stage), key, value};
        }
    }
}

/**
 * Reads the finished sandbox perk indices the stat block's second array declares.
 * These are rows of the sandbox perk table, not item indices. The character record wants that
 * form.
 * @param definition Whole item definition bytes.
 * @param row Receives the declared perk indices.
 */
void read_sandbox_perks(std::span<const std::byte> definition, Row& row) noexcept {
    std::size_t block = 0;
    Array perks{};
    if (!resolve_block(definition, kStatBlockOffset, block)
        || !find_array_at(definition, block + kSandboxPerkDescriptor, perks)
        || perks.elementClass != kSandboxPerkClass) {
        return;
    }
    for (std::uint64_t entry = 0; entry < perks.count; ++entry) {
        if (row.sandboxPerkCount >= kSandboxPerkCapacity) {
            return;
        }
        std::uint16_t perk = kUnavailableSandboxPerk;
        if (!read(definition,
                  perks.dataOffset + static_cast<std::size_t>(entry) * kSandboxPerkStride,
                  perk)) {
            return;
        }
        if (perk != kUnavailableSandboxPerk) {
            row.sandboxPerks[row.sandboxPerkCount++] = perk;
        }
    }
}

} // namespace

/** Reads the art indices, material override rows and sandbox perks one definition declares. */
void read_appearance(std::span<const std::byte> definition, Row& row) noexcept {
    row.gearArtIndex = kUnavailableArtIndex;
    row.artArrangementIndex = kUnavailableArtIndex;
    row.sandboxPerkCount = 0;
    row.renderOverrideCount = 0;
    std::size_t art = 0;
    // A definition with no art block draws nothing and declares no material overrides. Subclasses
    // and emblems take this path, and both art indices stay at their empty sentinel.
    if (resolve_block(definition, kArtBlockOffset, art)) {
        read_art(definition, art, row);
        read_material_overrides(definition, art, row);
    }
    read_sandbox_perks(definition, row);
}

} // namespace sunrise::middleware::content::packages::tables::items
