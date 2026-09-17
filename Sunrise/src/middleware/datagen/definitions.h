#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::middleware::datagen {

/** Native inspection family and its account/character object schemas. */
inline constexpr std::uint32_t kInspectionFamily = 1;
/** Object id for the family-one account slot. */
inline constexpr std::uint32_t kInspectionRootObjectId = 0xC47D63DBU;
/** Object id for the family-one selected-character slot. */
inline constexpr std::uint32_t kInspectionCharacterObjectId = 0x1A10412DU;

/** Object id the client matches against the family-three roster slot. */
inline constexpr std::uint32_t kRosterObjectId = 0x1C35451DU;
/** Object id for the family-three per-character record slot. */
inline constexpr std::uint32_t kRosterCharacterObjectId = 0x63E0C862U;
/** Object id for the family-zero banner anchor slot. */
inline constexpr std::uint32_t kBannerAnchorObjectId = 0xC6040EB0U;
/** Object id for the family-zero banner record slot. */
inline constexpr std::uint32_t kBannerCharacterObjectId = 0xD859602CU;
/** Object id for the family-four account slot. */
inline constexpr std::uint32_t kAccountObjectId = 0x32D7B974U;
/** Object id for the family-four selected-character slot. */
inline constexpr std::uint32_t kCharacterObjectId = 0xE5E86992U;
/** Object id for the family-four item-instance slot. */
inline constexpr std::uint32_t kItemInstanceObjectId = 0x6CFBA3ABU;

/** Object id for the family-five account unlock slot. */
inline constexpr std::uint32_t kUnlockObjectId = 0x8C4757F3U;
/** Family five is global, so its record and its one object share this fixed sentinel key. */
inline constexpr std::uint64_t kUnlockSentinelSoid = 0x7FFFFFFFFFFFFFFFULL;

/** Object id for the family-two social roster directory slot. */
inline constexpr std::uint32_t kSocialRosterDirectoryObjectId = 0xDA277CE4U;
/** Object id for the family-two social roster member slot. */
inline constexpr std::uint32_t kSocialRosterMemberObjectId = 0x811115CEU;
/** Object id for the family-six fireteam directory slot. */
inline constexpr std::uint32_t kFireteamDirectoryObjectId = 0xBF49D4F0U;
/** Object id for the family-six fireteam descriptor slot. */
inline constexpr std::uint32_t kFireteamDescriptorObjectId = 0xE40CA32AU;
/** Object id for the family-seven join directory slot. */
inline constexpr std::uint32_t kJoinDirectoryObjectId = 0x05D07598U;
/** Object id for the family-seven join descriptor slot. */
inline constexpr std::uint32_t kJoinDescriptorObjectId = 0x722C6528U;

/** Sizes the family-two slot descriptors declare, in bytes. */
inline constexpr std::size_t kSocialRosterDirectorySize = 96;
inline constexpr std::size_t kSocialRosterMemberSize = 80;

/** Families that carry a generated object. */
inline constexpr std::uint32_t kBannerFamily = 0;
inline constexpr std::uint32_t kSocialRosterFamily = 2;
inline constexpr std::uint32_t kRosterFamily = 3;
inline constexpr std::uint32_t kAccountFamily = 4;
inline constexpr std::uint32_t kUnlockFamily = 5;
inline constexpr std::uint32_t kFireteamFamily = 6;
inline constexpr std::uint32_t kJoinFamily = 7;

/** Slots those objects occupy. */
inline constexpr std::uint32_t kRosterSlot = 0;
inline constexpr std::uint32_t kSocialRosterDirectorySlot = 0;
inline constexpr std::uint32_t kSocialRosterMemberSlot = 1;
inline constexpr std::uint32_t kAccountSlot = 0;
inline constexpr std::uint32_t kCharacterSlot = 1;
inline constexpr std::uint32_t kItemInstanceSlot = 3;

/**
 * Finds the object id one family slot publishes.
 * @param familyType Queuez family.
 * @param slotIndex Slot inside that family.
 * @param objectId Receives the id.
 * @return True when the slot carries a generated object.
 */
[[nodiscard]] constexpr bool
object_id(std::uint32_t familyType, std::uint32_t slotIndex, std::uint32_t& objectId) noexcept {
    objectId = 0;
    if (familyType == kInspectionFamily && slotIndex == kAccountSlot) {
        objectId = kInspectionRootObjectId;
    } else if (familyType == kInspectionFamily && slotIndex == kCharacterSlot) {
        objectId = kInspectionCharacterObjectId;
    } else if (familyType == kInspectionFamily && slotIndex == kItemInstanceSlot) {
        objectId = kItemInstanceObjectId;
    } else if (familyType == kSocialRosterFamily && slotIndex == kSocialRosterDirectorySlot) {
        objectId = kSocialRosterDirectoryObjectId;
    } else if (familyType == kSocialRosterFamily && slotIndex == kSocialRosterMemberSlot) {
        objectId = kSocialRosterMemberObjectId;
    } else if (familyType == kFireteamFamily && slotIndex == 0) {
        objectId = kFireteamDirectoryObjectId;
    } else if (familyType == kFireteamFamily && slotIndex == 1) {
        objectId = kFireteamDescriptorObjectId;
    } else if (familyType == kJoinFamily && slotIndex == 0) {
        objectId = kJoinDirectoryObjectId;
    } else if (familyType == kJoinFamily && slotIndex == 1) {
        objectId = kJoinDescriptorObjectId;
    } else if (familyType == kRosterFamily && slotIndex == kRosterSlot) {
        objectId = kRosterObjectId;
    } else if (familyType == kAccountFamily && slotIndex == kAccountSlot) {
        objectId = kAccountObjectId;
    } else if (familyType == kAccountFamily && slotIndex == kCharacterSlot) {
        objectId = kCharacterObjectId;
    } else if (familyType == kAccountFamily && slotIndex == kItemInstanceSlot) {
        objectId = kItemInstanceObjectId;
    }
    return objectId != 0;
}

} // namespace sunrise::middleware::datagen
