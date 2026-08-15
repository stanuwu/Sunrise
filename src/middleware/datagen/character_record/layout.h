#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::datagen::character_record::layout {

/** An empty item slot is this sentinel; a 0 would assert definition index 0, a real item. */
inline constexpr std::uint16_t kEmptyDefinitionIndex = 0xFFFFU;
/** The engine's no-hash sentinel, and the empty value for every definition-hash field. */
inline constexpr std::uint32_t kNoHash = 0x811C9DC5U;
/** A signed key byte is empty at -1. */
inline constexpr std::int8_t kEmptyKey = -1;
/** An empty stat row carries -1, not 0, because 0 is a real stat total. */
inline constexpr std::int32_t kEmptyStatValue = -1;

/**
 * The fixed native header between the identity triple and the appearance block.
 * These are authored client-side ids, not extracted data. The empty fields carry their own
 * sentinels, so the block is written whole rather than field by field.
 */
inline constexpr std::array<std::uint8_t, 36> kHeaderBlockBytes{
    0x25, 0x00, 0x05, 0x00, 0x01, 0x00, 0xEA, 0x02, 0x01, 0x03, 0x46, 0x02,
    0x48, 0x03, 0x49, 0x03, 0x4A, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x13, 0x03,
    0xC7, 0x02, 0x00, 0x00, 0xC5, 0x9D, 0x1C, 0x81, 0x01, 0x00, 0x00, 0x00,
};

/** Race, gender and class occupy 3 signed key bytes after the character key. */
inline constexpr std::size_t kIdentityByteCount = 3;
/** The opaque native header between the identity triple and the appearance block. */
inline constexpr std::size_t kHeaderBlockSize = 36;
/** The subclass publishes 12 ability buckets with fixed meanings. */
inline constexpr std::size_t kAbilityBucketCapacity = 12;
/** Each ability bucket holds 16 definition hashes. */
inline constexpr std::size_t kBucketHashCapacity = 16;
/** One flat overflow hash bank follows the buckets, holding what no bucket category claims. */
inline constexpr std::size_t kOverflowHashCapacity = 32;
/** 3 art indices follow the render row's definition index. */
inline constexpr std::size_t kRenderArtCapacity = 3;
/** 8 overlay art indices follow them. */
inline constexpr std::size_t kRenderOverlayCapacity = 8;
/** 2 unlock pairs follow the overlays. */
inline constexpr std::size_t kUnlockPairCapacity = 2;
/** The gear render spec searches 6 material pairs by key. */
inline constexpr std::size_t kMaterialPairCapacity = 6;
/** The gear art index occupies the second art slot, the arrangement index the third. */
inline constexpr std::size_t kGearArtSlot = 1;
inline constexpr std::size_t kArtArrangementSlot = 2;
/** The equipped-render array is indexed by native equipment slot. */
inline constexpr std::size_t kRenderSlotCapacity = 20;
/** 8 keyed 64-bit rows follow the render array. */
inline constexpr std::size_t kKeyedRowCapacity = 8;
/** 48 definition indices follow the keyed rows. */
inline constexpr std::size_t kIndexBankCapacity = 48;
/** 3 banks of 16 definition indices follow. */
inline constexpr std::size_t kSmallIndexBankCapacity = 16;
/** Every stat table holds 32 keyed signed rows. */
inline constexpr std::size_t kStatRowCapacity = 32;
/** One character stat table comes before 3 per-weapon tables. */
inline constexpr std::size_t kWeaponStatTableCapacity = 3;

#pragma pack(push, 1)

/** One ability bucket: the item category it collects, then that category's definition hashes. */
struct AbilityBucket {
    std::int8_t kind{kEmptyKey};
    std::array<std::byte, 3> reserved{};
    std::array<std::uint32_t, kBucketHashCapacity> hashes{};
};

/** One unlock pair: a definition hash and the signed value the account state supplies. */
struct UnlockPair {
    std::uint32_t definitionHash{kNoHash};
    std::int32_t value{};
};

/** One material pair the gear render spec searches by key. */
struct MaterialPair {
    std::int8_t key{kEmptyKey};
    std::array<std::byte, 1> reserved{};
    std::uint16_t value{kEmptyDefinitionIndex};
};

/** One equipped-render row, indexed by native equipment slot. */
struct RenderEntry {
    std::uint64_t instanceSoid{};
    std::uint16_t definitionIndex{kEmptyDefinitionIndex};
    std::array<std::uint16_t, kRenderArtCapacity> art{};
    std::array<std::uint16_t, kRenderOverlayCapacity> overlays{};
    std::array<UnlockPair, kUnlockPairCapacity> unlockPairs{};
    std::array<MaterialPair, kMaterialPairCapacity> materialPairs{};
};

/** One keyed 64-bit row. */
struct KeyedRow {
    std::int8_t key{kEmptyKey};
    std::array<std::byte, 7> reserved{};
    std::uint64_t value{};
};

/** One keyed signed stat row. */
struct StatRow {
    std::int8_t key{kEmptyKey};
    std::array<std::byte, 3> reserved{};
    std::int32_t value{};
};

/** The appearance block shared byte-for-byte by the family-three and family-zero records. */
struct Appearance {
    std::int32_t level{};
    float unusedFloatA{};
    float unusedFloatB{};
    /** The light the banner shows, compared as a float by its only reader. */
    float light{};
    std::array<AbilityBucket, kAbilityBucketCapacity> abilityBuckets{};
    std::array<std::uint32_t, kOverflowHashCapacity> overflowHashes{};
    std::array<RenderEntry, kRenderSlotCapacity> render{};
    std::array<KeyedRow, kKeyedRowCapacity> keyedRows{};
    std::array<std::uint16_t, kIndexBankCapacity> indexBank{};
    std::array<std::uint16_t, kSmallIndexBankCapacity> smallBankA{};
    std::array<std::uint16_t, kSmallIndexBankCapacity> smallBankB{};
    std::array<std::uint16_t, kSmallIndexBankCapacity> smallBankC{};
    std::array<StatRow, kStatRowCapacity> characterStats{};
    std::array<std::array<StatRow, kStatRowCapacity>, kWeaponStatTableCapacity> weaponStats{};
    std::int32_t trailingValue{};
    std::array<std::byte, 4> trailingPadding{};
};

/** The identity prefix shared byte-for-byte by both records. */
struct Identity {
    std::uint64_t characterSoid{};
    std::array<std::int8_t, kIdentityByteCount> identity{};
    std::array<std::byte, 1> identityPadding{};
    std::array<std::byte, kHeaderBlockSize> headerBlock{};
};

/** The 32-byte summary block that follows the appearance block. */
struct Summary {
    float lightFloatA{};
    float lightFloatB{};
    std::int32_t light{};
    std::uint16_t indexA{kEmptyDefinitionIndex};
    std::uint16_t indexB{kEmptyDefinitionIndex};
    std::uint16_t indexC{kEmptyDefinitionIndex};
    std::array<std::byte, 2> reserved{};
    std::int32_t valueA{};
    std::uint32_t hashA{};
    std::uint16_t indexD{kEmptyDefinitionIndex};
    std::array<std::byte, 2> tailPadding{};
};

/**
 * The 24-byte tail only the family-zero record carries.
 * Every index-typed field here asserts definition 0 when left clear, so the tail is written
 * rather than zeroed even though none of its fields has a traced reader.
 */
struct Family0Tail {
    std::uint16_t indexA{kEmptyDefinitionIndex};
    std::uint16_t indexB{kEmptyDefinitionIndex};
    std::uint32_t hash{kNoHash};
    std::uint16_t indexC{kEmptyDefinitionIndex};
    std::uint16_t indexD{kEmptyDefinitionIndex};
    std::uint16_t indexE{kEmptyDefinitionIndex};
    std::uint16_t indexF{kEmptyDefinitionIndex};
    std::uint64_t reserved{};
};

#pragma pack(pop)

/** The identity prefix occupies the first 48 bytes of both records. */
inline constexpr std::size_t kIdentitySize = 48;
/** The appearance block occupies a fixed span in both records. */
inline constexpr std::size_t kAppearanceSize = 3'752;
/** The summary block follows the appearance block in both records. */
inline constexpr std::size_t kSummarySize = 32;
/** Each bank below is walked by its record stride, so every one must pack to an exact width. */
inline constexpr std::size_t kAbilityBucketSize = 68;
inline constexpr std::size_t kMaterialPairSize = 4;
inline constexpr std::size_t kUnlockPairSize = 8;
inline constexpr std::size_t kRenderEntrySize = 72;
inline constexpr std::size_t kKeyedRowSize = 16;
inline constexpr std::size_t kStatRowSize = 8;
/** The family-zero tail is the 24 bytes only that record carries. */
inline constexpr std::size_t kFamily0TailSize = 24;

/** Appearance bank positions, fixed by the record's wire layout rather than by C++ packing. */
struct AppearanceOffsets {
    /** The 12 ability buckets and their overflow count. */
    static constexpr std::size_t abilityBuckets = 16;
    /** Hash bank the buckets spill into once their fixed lanes are full. */
    static constexpr std::size_t overflowHashes = 832;
    /** Render array the character display reads its art from. */
    static constexpr std::size_t render = 960;
    /** Keyed progression rows. */
    static constexpr std::size_t keyedRows = 2'400;
    /** Trailing index bank, which keeps its native empty sentinels. */
    static constexpr std::size_t indexBank = 2'528;
    /** The 32-row character stat table, empty key 0xFF. */
    static constexpr std::size_t characterStats = 2'720;
    /** The weapon stat table that follows the character one. */
    static constexpr std::size_t weaponStats = 2'976;
};

static_assert(sizeof(AbilityBucket) == kAbilityBucketSize);
static_assert(sizeof(MaterialPair) == kMaterialPairSize);
static_assert(sizeof(UnlockPair) == kUnlockPairSize);
static_assert(sizeof(RenderEntry) == kRenderEntrySize);
static_assert(sizeof(KeyedRow) == kKeyedRowSize);
static_assert(sizeof(StatRow) == kStatRowSize);
static_assert(sizeof(Identity) == kIdentitySize);
static_assert(sizeof(Appearance) == kAppearanceSize);
static_assert(sizeof(Summary) == kSummarySize);
static_assert(sizeof(Family0Tail) == kFamily0TailSize);
static_assert(offsetof(Appearance, abilityBuckets) == AppearanceOffsets::abilityBuckets);
static_assert(offsetof(Appearance, overflowHashes) == AppearanceOffsets::overflowHashes);
static_assert(offsetof(Appearance, render) == AppearanceOffsets::render);
static_assert(offsetof(Appearance, keyedRows) == AppearanceOffsets::keyedRows);
static_assert(offsetof(Appearance, indexBank) == AppearanceOffsets::indexBank);
static_assert(offsetof(Appearance, characterStats) == AppearanceOffsets::characterStats);
static_assert(offsetof(Appearance, weaponStats) == AppearanceOffsets::weaponStats);
static_assert(std::is_standard_layout_v<Appearance>);
static_assert(std::is_trivially_copyable_v<Appearance>);

} // namespace sunrise::middleware::datagen::character_record::layout
