#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sunrise::middleware::content::packages::tables::items {

/** Ordinary socket lanes an item definition can declare. */
inline constexpr std::size_t kSocketCapacity = 12;
/** All bits set marks a socket lane with no initial plug. */
inline constexpr std::uint16_t kUnavailablePlug = 0xFFFF;
/** All bits set marks an item definition with no insertion or enable requirement set. */
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;
/** All bits set marks a completion value the objective block does not name. */
inline constexpr std::uint16_t kUnavailableValueSlot = 0xFFFFU;

/** Declared stat contributions one definition carries. Shipped rows stay far below this. */
inline constexpr std::size_t kStatCapacity = 64;
/** Sandbox perk indices one definition declares. The widest shipped item declares 4. */
inline constexpr std::size_t kSandboxPerkCapacity = 4;
/** Material override rows one definition declares over its 3 art stages. Widest shipped is 30. */
inline constexpr std::size_t kRenderOverrideCapacity = 32;
/** All bits set marks an art index the definition does not declare. */
inline constexpr std::uint16_t kUnavailableArtIndex = 0xFFFF;
/** Generic art plus one row for each of Titan, Hunter, and Warlock. */
inline constexpr std::size_t kArtClassCapacity = 4;
/** All bits set marks a socket lane whose type the definition does not declare. */
inline constexpr std::uint16_t kUnavailableSocketType = 0xFFFF;
/** A signed material override key is empty at minus one. */
inline constexpr std::int8_t kEmptyOverrideKey = -1;
/** The installed pursuit objective block supports seven displayed objectives. */
inline constexpr std::size_t kObjectiveCapacity = 7;
/** The item instance record ends in eight per-item values, so a pursuit maps at most eight. */
inline constexpr std::size_t kItemValueCapacity = 8;
/** An item's value block holds six fixed reward rows. */
inline constexpr std::size_t kRewardPreviewCapacity = 6;
/** All bits set marks a reward row that names no item or no condition. */
inline constexpr std::uint16_t kUnavailableRewardReference = 0xFFFF;
/** The shipped build lists at most 8 presence flags on one item. */
inline constexpr std::size_t kPresenceFlagCapacity = 8;

/** One material override row, tagged with the art stage that declared it. */
struct RenderOverride {
    std::uint8_t stage{};
    std::int8_t key{kEmptyOverrideKey};
    std::uint16_t value{};
};

/** One reward row a pursuit's value block declares. */
struct RewardPreview {
    std::uint16_t itemIndex{kUnavailableRewardReference};
    /** Zero when the row names its item without an amount. */
    std::int32_t quantity{};
    /** Shared expression row that decides whether the row applies, or unavailable for none. */
    std::uint16_t conditionIndex{kUnavailableRewardReference};
};

/** One item row read from its own definition blob. */
struct Row {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{};
    /** Native rarity ladder: 1 common through 5 exotic; 0 outside the ladder. */
    std::uint8_t tier{};
    std::int32_t maxStackSize{};
    bool instanced{};
    /** Bucket that receives what the item's own bucket cannot hold; all bits set for none. */
    std::uint8_t recoveryBucketId{};
    /** Acquisition records carry the policy bit that skips the toast and the acquired count. */
    bool quietAcquisition{};
    /** Acquisition records carry the policy bit that skips only the toast. */
    bool toastlessAcquisition{};
    /** The item's action removes the whole stack rather than one unit. */
    bool consumesWholeStack{};
    /** The item's action deletes the copy it acts on. */
    bool deletesOnAction{};
    /** FNV-1 hash of the stack-unique label string; the empty string's hash means none. */
    std::uint32_t stackUniqueLabel{};
    std::optional<std::int8_t> equipmentSlot{};
    std::int32_t rawEquipmentSlot{};
    bool hasSocketEntryList{};
    std::uint16_t socketEntryListIndex{};
    bool hasSockets{};
    std::uint8_t socketCount{};
    std::uint16_t initialPlugs[kSocketCapacity]{};
    std::uint16_t socketTypes[kSocketCapacity]{};
    /** Plug category used by a few native sockets to expand a seed into its whole safe family. */
    std::uint32_t plugCategoryHash{};
    /**
     * Ordinal of the server roll set that grants this plug in place of a socket's action plug;
     * 0 when the plug is socketed directly, 0xFFFF for a plug the service granted by other means.
     */
    std::uint16_t rollSetIndex{};
    /** Item index of the plug this one stands for, or kUnavailablePlug when it stands alone. */
    std::uint16_t linkedPlugIndex{kUnavailablePlug};
    /** Native material sets used when this definition is inserted or enabled as a plug. */
    std::uint16_t insertionMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    std::uint16_t enabledMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    /** Value slot a plug's quantity expression reads alone, or -1 when it is anything else. */
    std::int16_t quantityValueSlot{-1};
    /** Flag slot a plug's insertion rules together require set, or -1 for none or several. */
    std::int16_t insertionFlagSlot{-1};
    std::uint8_t statCount{};
    std::uint8_t statRows[kStatCapacity]{};
    std::int32_t statValues[kStatCapacity]{};
    /** Gear art definition index, read straight from the art block. */
    std::uint16_t gearArtIndex{kUnavailableArtIndex};
    /** Generic, Titan, Hunter, and Warlock art arrangements declared by the art block. */
    std::uint16_t artArrangementIndices[kArtClassCapacity]{};
    std::uint8_t sandboxPerkCount{};
    std::uint16_t sandboxPerks[kSandboxPerkCapacity]{};
    std::uint8_t renderOverrideCount{};
    RenderOverride renderOverrides[kRenderOverrideCapacity]{};
    /** Pursuit metadata is optional: an unreadable block leaves the identity row usable. */
    bool objectiveMetadataValid{true};
    /** A present objective block declares these completion controls, even without objectives. */
    bool hasCompletionMetadata{};
    bool requireAllObjectives{};
    bool requireNonOptionalCompletion{};
    /** Completed objectives never complete the pursuit. */
    bool suppressCompletion{};
    /** The pursuit completes as soon as its objectives do, without a request from the client. */
    bool automaticCompletion{};
    /** Value slots completion sets to one: the completion and completion-context values. */
    std::uint16_t completionValueSlot{kUnavailableValueSlot};
    std::uint16_t completionContextValueSlot{kUnavailableValueSlot};
    std::uint8_t objectiveCount{};
    std::uint16_t objectiveIndices[kObjectiveCapacity]{};
    /** Positive for a bounty, which expires this many seconds after it is acquired. */
    std::int32_t lifetimeSeconds{};
    /** A pursuit's per-item value slots in record order; none when its list cannot be read. */
    std::uint8_t itemValueSlotCount{};
    std::uint16_t itemValueSlots[kItemValueCapacity]{};
    /** A pursuit's value block is optional: an unreadable one leaves its rows unknown. */
    bool rewardPreviewsValid{true};
    std::uint8_t rewardPreviewCount{};
    RewardPreview rewardPreviews[kRewardPreviewCapacity]{};
    /** Unlock flag slots the item supplies while held; none when the list cannot be read. */
    std::uint8_t presenceFlagCount{};
    std::array<std::uint16_t, kPresenceFlagCapacity> presenceFlags{};
};

/**
 * Reads the art indices, material override rows and sandbox perks one definition declares.
 * All three live behind self-relative blob offsets and are valid only against the serialized
 * definition, never against a loaded definition in process memory.
 * @param definition Whole item definition bytes.
 * @param row Receives the art, material override and sandbox perk fields.
 */
void read_appearance(std::span<const std::byte> definition, Row& row) noexcept;

/**
 * Reads the fixed fields of one item definition blob.
 * @param definition Whole item definition bytes.
 * @param row Receives every field the blob declares.
 * @return True when the blob is long enough to carry the fixed fields.
 */
[[nodiscard]] bool read_definition(std::span<const std::byte> definition, Row& row) noexcept;

/**
 * Finds the insertion-rule array of a plug's definition. Each row is one unlock expression, and
 * every row must hold for the plug to be inserted.
 * @param definition Whole item definition bytes.
 * @param at Receives the offset of the array descriptor.
 * @return False for a definition with no plug block.
 */
[[nodiscard]] bool find_insertion_rules(std::span<const std::byte> definition,
                                        std::size_t& at) noexcept;

/** Visitor called for each native item-definition index an ordinary socket list names. */
using AllowedPlugVisitor = bool (*)(void* context, std::uint32_t itemDefinitionIndex) noexcept;

/**
 * Visits the embedded, reusable, and randomized plug-list members declared for one socket lane.
 * The initial plug is a separate fixed field and is intentionally left to the caller.
 * @param definition Whole base-item definition bytes.
 * @param plugSetTable Whole shared plug-set definition table from investment-root slot 51.
 * @param lane Ordinary socket lane to inspect.
 * @param visitor Required bounded consumer.
 * @param context Opaque consumer state.
 * @return True when every referenced array is structurally valid and accepted by the visitor.
 */
[[nodiscard]] bool visit_allowed_plugs(std::span<const std::byte> definition,
                                       std::span<const std::byte> plugSetTable,
                                       std::uint8_t lane,
                                       AllowedPlugVisitor visitor,
                                       void* context) noexcept;

/**
 * Visitor called for each shared plug-set member with the one flag slot its condition requires
 * set, or -1 when the condition requires none or more than one.
 */
using PlugSetFlagVisitor = void (*)(void* context,
                                    std::uint32_t itemDefinitionIndex,
                                    std::int16_t flagSlot) noexcept;

/**
 * Visits every member of every shared plug set with the flag its condition requires set.
 * @param plugSetTable Whole shared plug-set definition table from investment-root slot 51.
 * @return True when every set, member and condition parses.
 */
[[nodiscard]] bool visit_plug_set_flags(std::span<const std::byte> plugSetTable,
                                        PlugSetFlagVisitor visitor,
                                        void* context) noexcept;

} // namespace sunrise::middleware::content::packages::tables::items
