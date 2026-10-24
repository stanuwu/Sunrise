#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Family zero carries the banner anchor and the record for the character it names. */
inline constexpr std::uint32_t kBannerFamilyType = 0;
/** Family two carries the social roster the Roster and Fireteam panels draw. */
inline constexpr std::uint32_t kSocialRosterFamilyType = 2;
/** Family three carries the account character roster. */
inline constexpr std::uint32_t kRosterFamilyType = 3;
/** Family four carries account, character, and item state. */
inline constexpr std::uint32_t kAccountFamilyType = 4;
/** Initial and replayed full snapshots use version zero. */
inline constexpr std::int32_t kInitialFamilyVersion = 0;
/**
 * Family four holds account, character, one id per character-owned item, and one id per
 * resident-backed mod or shader stack. Profile currency rows carry no instance SOID; the fixed
 * addition covers all 50 native rows in each supported action-source bucket.
 */
inline constexpr std::size_t kResidentCapacity =
    2 + state::kCharacterCapacity * middleware::datagen::family4::loadout::kItemCapacity
    + state::account::inventory::kProfileActionSourceCapacity;
/** Maximum inventory-row identities retained while an acquisition feed is visible. */
inline constexpr std::size_t kAcquisitionPresentationRowCapacity = 16;

/** One item identity pinned to the inventory row referenced by the active acquisition feed. */
struct AcquisitionPresentationRow {
    std::uint64_t instanceSoid{};
    std::uint16_t inventoryRow{};
};

/** Whether a Family-3 subscription still gets a body: always, once more, or never again. */
enum class Family3Phase : std::uint8_t {
    normal,
    publishOnce,
    responseOnly,
};

/** Stable id of one object now resident in this peer's Family-4 store. */
struct ResidentObject {
    std::uint64_t objectSoid{};
    std::uint32_t definitionId{};
};

/** Fixed queuez transaction state owned by one authenticated BAP peer. */
struct SessionState {
    std::uint64_t family4RootSoid{};
    /** Root whose Family-3 roster store has accepted its full snapshot. */
    std::uint64_t family3RootSoid{};
    /** Resident zero is the account; the character is found by definition id. */
    std::array<ResidentObject, kResidentCapacity> family4Residents{};
    /** Character the resident family-zero pair names. Only a change earns an incremental. */
    std::uint64_t family0Character{};
    std::int32_t family4Version{};
    /** Version zero is the full roster; every accepted appearance refresh adds exactly one. */
    std::int32_t family3Version{};
    /** Retail sets the full-snapshot flag once per family, then increments this by one. */
    std::int32_t family0Version{};
    /** Every family-five publication is a full snapshot, so its version only has to move. */
    std::int32_t family5Version{};
    std::uint16_t family4ResidentCount{};
    Family3Phase family3Phase{Family3Phase::normal};
    bool family4Active{};
    /** Set only after a complete Family-3 full-snapshot frame has been staged for publication. */
    bool family3Active{};
    /** Set once the family-zero full snapshot has been published to this peer. */
    bool family0Active{};
    /**
     * Root of a family-zero subscription that arrived before any character was selected.
     * The peer never asks again, so the held root is answered at the first pick. Zero once sent.
     */
    std::uint64_t pendingBannerRoot{};
};

/** Validated opcode-505 after-image and its resident account definition. */
struct ChangeCharacter {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
};

/**
 * Validated opcode-504 after-image and the three object operations its Family-4 move needs.
 * The character object is deleted at its old key before it is upserted at the new one. One slot
 * holds it, and an upsert onto a full slot raises queuez error 4.
 */
struct SelectCharacter {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    std::uint32_t characterDefinitionId{};
    std::uint64_t previousCharacterSoid{};
    std::uint64_t selectedCharacterSoid{};
    /**
     * The account object moves as a selected-character patch, not a full body.
     * The first pick replaces it whole. After opcode 505, or on a re-pick of the resident
     * character, a resent account body wipes the settings block, so only the one field goes out.
     */
    bool patchAccount{};
};

/** Validated opcode-403 after-image for a single resident character upsert. */
struct EquipmentSwap {
    SessionState after{};
    std::uint32_t characterDefinitionId{};
    std::uint64_t characterSoid{};
};

/**
 * Validated Family-0 after-image for the appearance record changed by one equipment swap.
 *
 * The resident character key does not move: the update upserts that one record in place.
 */
struct CharacterAppearanceRefresh {
    SessionState after{};
    std::uint64_t characterSoid{};
};

/** Validated Family-3 appearance after-image for one character and optional account roster. */
struct RosterAppearanceRefresh {
    SessionState after{};
    std::uint64_t characterSoid{};
    /** Equipment moves change the roster's slot definition; socket-only changes do not. */
    bool includeRoster{};
};

/** Validated item-acquisition after-image for one character upsert and one new instance object. */
struct ItemAcquisition {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    std::uint32_t characterDefinitionId{};
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t acquiredInstanceSoid{};
    /** True when the same revision also publishes the charged profile-material balances. */
    bool updatesAccount{};
    std::uint64_t evictedInstanceSoid{};
};

/** Validated socket-action after-image for one item upsert and optional material balances. */
struct SocketPlug {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t targetInstanceSoid{};
    bool updatesAccount{};
};

/** Validated subclass item-instance after-image for one ability socket-entry selection. */
struct SubclassSelection {
    SessionState after{};
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t subclassInstanceSoid{};
};

/** Validated profile-stack acquisition after-image for an account upsert and optional resident. */
struct ProfileItemAcquisition {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    /** Family-4 slot-three schema used by a resident-backed shader or modification stack. */
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    /** Stable profile-row identity, or zero for a non-actionable currency/material stack. */
    std::uint64_t acquiredInstanceSoid{};
    /** Shared installed-build classifier copied from the prepared State mutation. */
    bool actionSource{};
    /** True only when this revision appends the resident at the prior manifest tail. */
    bool appendedResident{};
};

/** One Family-4 increment for every item row and the accompanying record claim. */
struct RecordRewardGrant {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    std::uint32_t characterDefinitionId{};
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::size_t appendedResidentCount{};
    std::array<std::uint64_t, state::kRecordRewardGrantCapacity> releasedResidents{};
    std::size_t releasedResidentCount{};
};

/** Validated item-dismantle after-image for one character upsert and one instance release. */
struct ItemDismantle {
    SessionState after{};
    std::uint32_t accountDefinitionId{};
    std::uint32_t characterDefinitionId{};
    std::uint32_t itemInstanceDefinitionId{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t dismantledInstanceSoid{};
    /** True when the same revision also publishes credited profile materials. */
    bool updatesAccount{};
};

/** Queuez fields published after every staged frame is copied to caller output. */
struct StagedPublication {
    SessionState after{};
    bool hasState{};
    /** The Family-4 companion went out and owes its delayed second copy. */
    bool armsFamily4Repush{};
    /** Root the companion used, kept because an unmapped snapshot records no residents. */
    std::uint64_t family4RepushRoot{};
    /**
     * Root a family-two subscribe was answered against, or zero when this frame answered none.
     * A subscribe is the only moment that root arrives, so the connection keeps the last one.
     */
    std::uint64_t socialRosterRepushRoot{};
    /** An emblem equip left the published family-two object stale and it owes a fresh copy. */
    bool rearmsSocialRosterRepush{};
    /** A subclass selection just staged and owes a delayed ability-icon refresh. */
    bool armsAbilityRefresh{};
    /** Complete row-identity overlay to retain after this equipment transaction commits. */
    std::array<AcquisitionPresentationRow, kAcquisitionPresentationRowCapacity>
        acquisitionPresentationRows{};
    std::uint8_t acquisitionPresentationRowCount{};
    bool updatesAcquisitionPresentationRows{};
};

} // namespace sunrise::server::bap::encrypted::queuez
