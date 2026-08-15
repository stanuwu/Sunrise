#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../state/account/account_state.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Family zero carries the banner anchor and the record for the character it names. */
inline constexpr std::uint32_t kBannerFamilyType = 0;
/** Family three carries the account character roster. */
inline constexpr std::uint32_t kRosterFamilyType = 3;
/** Family four carries account, character, and item state. */
inline constexpr std::uint32_t kAccountFamilyType = 4;
/** Initial and replayed full snapshots use version zero. */
inline constexpr std::int32_t kInitialFamilyVersion = 0;
/**
 * Family four holds account, character, and one id per equipped item on every character.
 * It matches the snapshot descriptor size, so a snapshot that builds always stages.
 */
inline constexpr std::size_t kResidentCapacity =
    2 + state::kCharacterCapacity * middleware::datagen::family4::loadout::kItemCapacity;
/** Resident zero is the account object. The character object is found by its definition id. */

/** When the roster is published after a change, as measured in the character-select flow. */
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
    std::array<ResidentObject, kResidentCapacity> family4Residents{};
    /** Character the resident family-zero pair names. Only a change earns an incremental. */
    std::uint64_t family0Character{};
    std::int32_t family4Version{};
    /** Retail sets the full-snapshot flag once per family, then increments this by one. */
    std::int32_t family0Version{};
    std::uint8_t family4ResidentCount{};
    Family3Phase family3Phase{Family3Phase::normal};
    bool family4Active{};
    /** Set once the family-zero full snapshot has been published to this peer. */
    bool family0Active{};
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
     * The first pick replaces it whole, which is the measured path. After opcode 505 a resent
     * account body wipes the resident settings block, so only the one field goes out.
     */
    bool patchAccount{};
};

/** Queuez fields published after every staged frame is copied to caller output. */
struct StagedPublication {
    SessionState after{};
    bool hasState{};
    /** The Family-4 companion went out and owes its delayed second copy. */
    bool armsFamily4Repush{};
    /** Root the companion used, kept because an unmapped snapshot records no residents. */
    std::uint64_t family4RepushRoot{};
};

} // namespace sunrise::server::bap::encrypted::queuez
