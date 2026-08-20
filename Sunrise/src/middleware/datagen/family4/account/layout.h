#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../../roster/queuez_roster.h"
#include "../inventory/layout.h"
#include "../progression/layout.h"
#include "preferences/layout.h"

namespace sunrise::middleware::datagen::family4::account::layout {

/** The account profile inventory reserves 701 fixed native rows. */
inline constexpr std::size_t kProfileItemCapacity = 701;
/** A secondary account inventory reserves 6 fixed native rows. */
inline constexpr std::size_t kSecondaryItemCapacity = 6;
/** The account progression bank reserves 127 fixed native rows. */
inline constexpr std::size_t kProgressionCapacity = 127;
/** The account publicity bank reserves 128 fixed session deadlines. */
inline constexpr std::size_t kPublicityExpiryCapacity = 128;
/** 52 replicated bytes cover every account seen-message bit. */
inline constexpr std::size_t kSeenMessageByteCount = 52;
/** The remaining native account header occupies 128 bytes before the roster. */
inline constexpr std::size_t kAccountHeaderTailSize = 128;
/** 8 reserved bytes separate the fixed roster from the selected-character key. */
inline constexpr std::size_t kRosterSelectionPaddingSize = 8;
/** 24 reserved bytes separate selection from publicity deadlines. */
inline constexpr std::size_t kSelectionPublicityPaddingSize = 24;
/** 20 reserved bytes separate seen messages from the profile-setup completion byte. */
inline constexpr std::size_t kSeenProfileSetupPaddingSize = 20;
/** 3 reserved bytes align account preferences after the profile-setup completion byte. */
inline constexpr std::size_t kProfileSetupPreferencesPaddingSize = 3;
/** 607 reserved bytes separate preference and keybinding records. */
inline constexpr std::size_t kPreferencesBindingsPaddingSize = 607;
/** 456 reserved bytes follow the replicated keybinding record. */
inline constexpr std::size_t kBindingsProfilePaddingSize = 456;
/** Native inventory counts are followed by 4 reserved alignment bytes. */
inline constexpr std::size_t kInventoryCountPaddingSize = 4;
/** The profile inventory observer reads 16 transient mutation descriptors. */
inline constexpr std::size_t kProfileInventoryChangeRecordCapacity = 16;
/** Opaque bytes after the profile mutation bank keep the progression bank at its native offset. */
inline constexpr std::size_t kProfileInventoryProgressionPaddingSize = 512;
/** The account acquired-flag bank holds one byte per flag. */
inline constexpr std::size_t kAcquiredFlagCapacity = 12'300;
/** The account objective-value bank holds one signed value per objective. */
inline constexpr std::size_t kObjectiveValueCapacity = 6'200;
/** The unlock bank reserves 4 fixed per-character blocks. */
inline constexpr std::size_t kUnlockCharacterCapacity = 4;
/** Each per-character block holds 256 acquired flags. */
inline constexpr std::size_t kCharacterFlagCapacity = 256;
/** Each per-character block holds 256 objective values. */
inline constexpr std::size_t kCharacterValueCapacity = 256;
/** The profile unlock bank holds 512 acquired flags. */
inline constexpr std::size_t kProfileUnlockFlagCapacity = 512;
/** Opaque records separate the per-character blocks from the profile unlock bank. */
inline constexpr std::size_t kUnlockProfilePaddingSize = 2'424;
/** Opaque records follow the profile unlock bank to the end of the object. */
inline constexpr std::size_t kProfileFlagsTailPaddingSize = 21'384;
/** The complete native Family-4 account object occupies 96,280 bytes. */
inline constexpr std::size_t kObjectSize = 96'280;
/** The account key starts the native Family-4 account object. */
inline constexpr std::size_t kAccountSoidOffset = 0;
/** The fixed native roster begins after the account key and opaque header. */
inline constexpr std::size_t kRosterOffset = 136;
/** The selected-character key follows the complete fixed native roster. */
inline constexpr std::size_t kSelectedCharacterSoidOffset = 1'832;
/** The fixed publicity deadline bank begins after selection padding. */
inline constexpr std::size_t kPublicityExpiriesOffset = 1'864;
/** The seen-message bit bank follows all fixed publicity deadlines. */
inline constexpr std::size_t kSeenMessagesOffset = 2'888;
/** One byte at native offset 0xB90 records whether the one-time profile setup is complete. */
inline constexpr std::size_t kProfileSetupCompletedOffset = 2'960;
/** The native preference record follows the seen-message padding. */
inline constexpr std::size_t kPreferencesOffset = 2'964;
/** The replicated keybinding record follows its fixed preference padding. */
inline constexpr std::size_t kBindingsOffset = 3'648;
/** The profile inventory count begins the first fixed inventory bank. */
inline constexpr std::size_t kProfileItemCountOffset = 4'360;
/** The profile inventory rows follow their count and alignment bytes. */
inline constexpr std::size_t kProfileItemsOffset = 4'368;
/** The secondary inventory count follows every fixed profile row. */
inline constexpr std::size_t kSecondaryItemCountOffset = 26'800;
/** The secondary inventory rows follow their count and alignment bytes. */
inline constexpr std::size_t kSecondaryItemsOffset = 26'808;
/** The profile-inventory mutation bank follows the complete secondary inventory. */
inline constexpr std::size_t kProfileInventoryChangesOffset = 27'000;
/** The fixed progression bank precedes the account acquired-flag bank. */
inline constexpr std::size_t kProgressionsOffset = 27'708;
/** The account acquired-flag bank follows every fixed progression row. */
inline constexpr std::size_t kAcquiredFlagsOffset = 29'740;
/** The account objective-value bank follows every acquired flag. */
inline constexpr std::size_t kObjectiveValuesOffset = 42'040;
/** The 4 per-character unlock blocks follow the account objective values. */
inline constexpr std::size_t kCharacterUnlocksOffset = 66'840;
/** The profile unlock bank follows the opaque records after the per-character blocks. */
inline constexpr std::size_t kProfileUnlockFlagsOffset = 74'384;

#pragma pack(push, 1)

/** One character's acquired flags and objective values inside the account unlock bank. */
struct CharacterUnlockBlock {
    std::array<std::uint8_t, kCharacterFlagCapacity> flags{};
    std::array<std::int32_t, kCharacterValueCapacity> values{};
};

/** One transient profile inventory mutation consumed by the native account-object observer. */
struct ProfileInventoryChangeRecord {
    std::uint16_t sequence{};
    std::uint16_t reserved{};
    /** Mutation serial of the profile inventory row this record describes. */
    std::int32_t mutationSerial{};
    /** Nonzero mutation kind. Kind 1 follows the ordinary acquisition path. */
    std::uint8_t kind{};
    std::uint8_t reservedKind{};
    /** Native observer policy bits; 0 enables the ordinary acquisition path. */
    std::uint16_t flags{};
};

/** Header and fixed record bank beginning at native account-object offset 0x6978. */
struct ProfileInventoryChangeList {
    std::uint16_t writeSlot{};
    std::uint16_t nextSequence{};
    std::array<ProfileInventoryChangeRecord, kProfileInventoryChangeRecordCapacity> records{};
};

/** Byte-exact Family-4 account object generated from State. */
struct Object {
    std::uint64_t accountSoid{};
    std::array<std::byte, kAccountHeaderTailSize> accountHeaderTail{};
    roster::Block roster{};
    std::array<std::byte, kRosterSelectionPaddingSize> rosterSelectionPadding{};
    std::uint64_t selectedCharacterSoid{};
    std::array<std::byte, kSelectionPublicityPaddingSize> selectionPublicityPadding{};
    std::array<std::uint64_t, kPublicityExpiryCapacity> publicityExpiries{};
    std::array<std::byte, kSeenMessageByteCount> seenMessages{};
    std::array<std::byte, kSeenProfileSetupPaddingSize> seenProfileSetupPadding{};
    std::uint8_t profileSetupCompleted{};
    std::array<std::byte, kProfileSetupPreferencesPaddingSize> profileSetupPreferencesPadding{};
    preferences::Record preferences{};
    std::array<std::byte, kPreferencesBindingsPaddingSize> preferencesBindingsPadding{};
    preferences::BindingsRecord bindings{};
    std::array<std::byte, kBindingsProfilePaddingSize> bindingsProfilePadding{};
    std::uint32_t profileItemCount{};
    std::array<std::byte, kInventoryCountPaddingSize> profileCountPadding{};
    std::array<inventory::layout::Entry, kProfileItemCapacity> profileItems{};
    std::uint32_t secondaryItemCount{};
    std::array<std::byte, kInventoryCountPaddingSize> secondaryCountPadding{};
    std::array<inventory::layout::Entry, kSecondaryItemCapacity> secondaryItems{};
    ProfileInventoryChangeList profileInventoryChanges{};
    std::array<std::byte, kProfileInventoryProgressionPaddingSize>
        profileInventoryProgressionPadding{};
    std::array<progression::layout::Entry, kProgressionCapacity> progressions{};
    std::array<std::uint8_t, kAcquiredFlagCapacity> acquiredFlags{};
    std::array<std::int32_t, kObjectiveValueCapacity> objectiveValues{};
    std::array<CharacterUnlockBlock, kUnlockCharacterCapacity> characterUnlocks{};
    std::array<std::byte, kUnlockProfilePaddingSize> unlockProfilePadding{};
    std::array<std::uint8_t, kProfileUnlockFlagCapacity> profileUnlockFlags{};
    std::array<std::byte, kProfileFlagsTailPaddingSize> profileFlagsTailPadding{};
};

#pragma pack(pop)

/** The minimum encoded span is the complete typed account object. */
inline constexpr std::size_t kMinimumSize = kObjectSize;

static_assert(sizeof(Object) == kObjectSize);
static_assert(offsetof(Object, accountSoid) == kAccountSoidOffset);
static_assert(offsetof(Object, roster) == kRosterOffset);
static_assert(offsetof(Object, selectedCharacterSoid) == kSelectedCharacterSoidOffset);
static_assert(offsetof(Object, publicityExpiries) == kPublicityExpiriesOffset);
static_assert(offsetof(Object, seenMessages) == kSeenMessagesOffset);
static_assert(offsetof(Object, profileSetupCompleted) == kProfileSetupCompletedOffset);
static_assert(offsetof(Object, preferences) == kPreferencesOffset);
static_assert(offsetof(Object, bindings) == kBindingsOffset);
static_assert(offsetof(Object, profileItemCount) == kProfileItemCountOffset);
static_assert(offsetof(Object, profileItems) == kProfileItemsOffset);
static_assert(offsetof(Object, secondaryItemCount) == kSecondaryItemCountOffset);
static_assert(offsetof(Object, secondaryItems) == kSecondaryItemsOffset);
static_assert(offsetof(Object, profileInventoryChanges) == kProfileInventoryChangesOffset);
static_assert(offsetof(Object, progressions) == kProgressionsOffset);
static_assert(offsetof(Object, acquiredFlags) == kAcquiredFlagsOffset);
static_assert(offsetof(Object, objectiveValues) == kObjectiveValuesOffset);
static_assert(offsetof(Object, characterUnlocks) == kCharacterUnlocksOffset);
static_assert(offsetof(Object, profileUnlockFlags) == kProfileUnlockFlagsOffset);
static_assert(sizeof(CharacterUnlockBlock)
              == kCharacterFlagCapacity + kCharacterValueCapacity * sizeof(std::int32_t));
static_assert(sizeof(ProfileInventoryChangeRecord)
              == 3 * sizeof(std::uint16_t) + sizeof(std::int32_t) + 2 * sizeof(std::uint8_t));
static_assert(sizeof(ProfileInventoryChangeList)
              == 2 * sizeof(std::uint16_t)
                     + kProfileInventoryChangeRecordCapacity
                           * sizeof(ProfileInventoryChangeRecord));
static_assert(std::is_standard_layout_v<Object>);
static_assert(std::is_trivially_copyable_v<Object>);

} // namespace sunrise::middleware::datagen::family4::account::layout
