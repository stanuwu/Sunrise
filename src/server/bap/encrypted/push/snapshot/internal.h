#pragma once

#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/build_data/definition.h"
#include "../../../../../state/equipment/light/definition.h"
#include "snapshot.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Initial family snapshots start at version zero. */
inline constexpr std::int32_t kInitialFamilyVersion = 0;
/** Family three carries the account roster selected by Web Service subscription. */
inline constexpr std::uint32_t kRosterFamilyType = 3;
/** Family four carries account and selected-character investment state. */
inline constexpr std::uint32_t kAccountFamilyType = 4;
/** Descriptor slot zero owns the family-three account roster object. */
inline constexpr std::uint32_t kRosterDefinitionSlotIndex = 0;
/** Descriptor slot zero owns the family-four account object. */
inline constexpr std::uint32_t kAccountDefinitionSlotIndex = 0;
/** Descriptor slot one owns the family-four selected-character object. */
inline constexpr std::uint32_t kCharacterDefinitionSlotIndex = 1;
/** Descriptor slot three supplies the schema shared by every family-four item instance. */
inline constexpr std::uint32_t kItemDefinitionSlotIndex = 3;
/** Prepared descriptor zero carries the family-three roster object. */
inline constexpr std::size_t kRosterObjectIndex = 0;
/** Prepared descriptor zero carries the family-four account object. */
inline constexpr std::size_t kAccountObjectIndex = 0;
/** Prepared descriptor one carries the selected family-four character object. */
inline constexpr std::size_t kCharacterObjectIndex = 1;
/** Prepared item descriptors start after the account and selected-character descriptors. */
inline constexpr std::size_t kFirstItemObjectIndex = kFamily4IdentityObjectCount;
/**
 * Prepared item descriptors start here when no character is selected.
 * Item records cover every character in the roster, so they go out with or without a selection.
 * The account object is then the only descriptor ahead of them.
 */
inline constexpr std::size_t kFirstItemObjectIndexUnselected = kAccountObjectIndex + 1;
/** Roster and account-only snapshots each contain one object. */
inline constexpr std::size_t kSingleObjectCount = 1;

/**
 * Builds the family-three account roster snapshot.
 * @param scratch Object storage owned by the lock.
 * @param subscription Family id the Client picked.
 * @param objectId Id the roster object publishes under.
 * @param reservation Prior payload prefixes that staging must keep.
 * @param prepared Gets the roster descriptor and the scratch clear extent.
 * @return True when State and the mapped object size fit.
 */
[[nodiscard]] bool prepare_roster(Scratch& scratch,
                                  const middleware::queuez::Subscription& subscription,
                                  std::uint32_t objectId,
                                  const Reservation& reservation,
                                  Prepared& prepared) noexcept;

/**
 * Compresses one encoded family-four object into the next sealed scratch segment.
 * @param scratch Raw and compressed snapshot storage owned by the lock.
 * @param encoded The encoded object bytes, in the raw scratch prefix.
 * @param definitionId Descriptor id mapped at runtime.
 * @param version Object SOID, used as the first object version.
 * @param destinationOffset First unused byte of compressed scratch.
 * @param object Gets a descriptor only after compression works.
 * @param written Gets the compressed size in bytes.
 * @return True when the whole stream fits the remaining sealed storage.
 */
[[nodiscard]] bool compress_object(Scratch& scratch,
                                   std::span<const std::byte> encoded,
                                   std::uint32_t definitionId,
                                   std::uint64_t version,
                                   std::size_t destinationOffset,
                                   middleware::queuez::Object& object,
                                   std::size_t& written) noexcept;

/**
 * Builds the Family-4 account, selected-character, and item-instance snapshot.
 * @param scratch Object and compression storage owned by the lock.
 * @param subscription Family id the Client picked.
 * @param accountObjectId Id the account object publishes under.
 * @param reservation Prior payload prefixes that staging must keep.
 * @param prepared Gets the compressed object descriptors and scratch clear extents.
 * @return True when State, mappings, layouts and the installed compression all fit.
 */
[[nodiscard]] bool prepare(Scratch& scratch,
                           const middleware::queuez::Subscription& subscription,
                           std::uint32_t accountObjectId,
                           const Reservation& reservation,
                           Prepared& prepared) noexcept;

/**
 * Builds the Family-4 increment that moves the character object to the picked character.
 * @param scratch Object and compression storage owned by the lock.
 * @param select Checked after-image, object definitions and both character keys.
 * @param prepared Gets the release and both upsert descriptors.
 * @return True when State, mappings, layouts and the installed compression all fit.
 */
[[nodiscard]] bool prepare_selection_move(Scratch& scratch,
                                          const queuez::SelectCharacter& select,
                                          Prepared& prepared) noexcept;

/** Selected-character mappings the character and item-instance encoders need. */
struct Resolved {
    std::size_t characterIndex{};
    middleware::datagen::family4::loadout::ResolvedLoadout loadout{};
    state::equipment::light::Evaluation lightEvaluation{};
    std::uint32_t characterObjectId{};
    std::uint32_t itemInstanceObjectId{};
};

/**
 * Finds the selected character row inside one validated account snapshot.
 * @param account Account State read under the lock.
 * @return Row index, or no value when nothing is selected.
 */
[[nodiscard]] std::optional<std::size_t>
find_character_index(const state::AccountState& account) noexcept;

/**
 * Finds one selected row plus its character schema and, if items exist, the instance schema.
 * @param account Validated account State read under the lock.
 * @param characterIndex Selected row in the used part of the character array.
 * @param output Gets the mappings only after every lookup works.
 * @return True when the loadout and queuez mappings fit the encoder ABIs.
 */
[[nodiscard]] bool
resolve(const state::AccountState& account, std::size_t characterIndex, Resolved& output) noexcept;

/** Logs which step of preparation failed. @return Always false. */
[[nodiscard]] bool report_failure(const char* step) noexcept;

/**
 * Compresses one raw object and advances the shared sealed-buffer extent.
 * @param scratch Raw and compressed storage owned by the lock.
 * @param encoded The raw object bytes.
 * @param definitionId Descriptor id mapped at runtime.
 * @param version Object SOID, used as its first version.
 * @param object Gets the finished object descriptor.
 * @param compressedExtent First unused sealed byte, advanced on success.
 * @return True when the object fits the remaining sealed storage.
 */
[[nodiscard]] bool append_object(Scratch& scratch,
                                 std::span<const std::byte> encoded,
                                 std::uint32_t definitionId,
                                 std::uint64_t version,
                                 middleware::queuez::Object& object,
                                 std::size_t& compressedExtent) noexcept;

/**
 * Encodes and appends one character's row-sorted item instances after any already staged.
 * @param scratch Raw and compressed storage owned by the lock.
 * @param rawStorage Writable raw staging tail after any prior live payload.
 * @param itemInstanceObjectId Id each item object publishes under.
 * @param instances Every found item instance for one character.
 * @param baseIndex First descriptor slot the items may use. It shifts with whether the
 * selected-character descriptor sits ahead of them.
 * @param staged Prepared snapshot that takes the item descriptors from the base index on.
 * @param itemCursor Item descriptors already staged, advanced for every item.
 * @param compressedExtent First unused sealed byte, advanced for every item.
 * @return True when every item encodes and compresses with nothing half-published.
 */
[[nodiscard]] bool
append_items(Scratch& scratch,
             std::span<std::byte> rawStorage,
             std::uint32_t itemInstanceObjectId,
             const middleware::datagen::family4::loadout::ResolvedInstances& instances,
             std::size_t baseIndex,
             Prepared& staged,
             std::size_t& itemCursor,
             std::size_t& compressedExtent) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
