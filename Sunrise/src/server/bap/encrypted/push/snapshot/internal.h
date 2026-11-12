#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/build_data/definition.h"
#include "../../../../../state/equipment/light/definition.h"
#include "snapshot.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Builds the family-seven join descriptor naming this account's directory key. */
[[nodiscard]] bool prepare_join_descriptor(Scratch& scratch,
                                           const middleware::queuez::Subscription& subscription,
                                           const Reservation& reservation,
                                           Prepared& prepared) noexcept;

/** Builds the family-six fireteam snapshot naming this account's fireteam directory key. */
/**
 * Resolves the selected character's equipped emblem to a native definition index.
 * @param index Receives the native definition index; unchanged when no emblem resolves.
 * @param definitionHash Receives the equipped emblem hash; unchanged when no emblem resolves.
 */
void read_selected_emblem(const state::AccountState& account,
                          std::uint16_t& index,
                          std::uint32_t& definitionHash) noexcept;
[[nodiscard]] bool prepare_fireteam(Scratch& scratch,
                                    const middleware::queuez::Subscription& subscription,
                                    const Reservation& reservation,
                                    Prepared& prepared) noexcept;

/** Builds the family-one equipment inspection snapshot for the subscribed root account, not
    the connection's own. */
[[nodiscard]] bool prepare_inspection(Scratch& scratch,
                                      const middleware::queuez::Subscription& subscription,
                                      const Reservation& reservation,
                                      Prepared& prepared) noexcept;

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

/** @return True when one change record is unwritten, which is how every snapshot encodes it. */
inline constexpr auto kChangeRecordIsZero = [](const auto& record) noexcept {
    return record.sequence == 0 && record.reserved == 0 && record.mutationSerial == 0
           && record.kind == 0 && record.reservedKind == 0 && record.flags == 0;
};

/**
 * The change ring the account observer reads. A gain it does not name draws no pickup feedback.
 * Local to one incremental upsert; an ordinary snapshot encodes it empty.
 */
inline constexpr std::uint8_t kChangeKind = 1;
/**
 * The client checks vendor-transfer room under this kind, and its observer presents a row moved
 * under it without counting a new acquisition.
 */
inline constexpr std::uint8_t kVendorTransferChangeKind = 0xD;
/** Clear policy bits leave the record enabled; the observer skips any other pair. */
inline constexpr std::uint16_t kChangeFlags = 0;
/**
 * The client's record writer sets this on a new row whose collectible it has not yet acquired; its
 * observer then adds the collectible to its list of newly acquired ones.
 */
inline constexpr std::uint16_t kNewCollectibleChangeFlag = 0x10;
/** The client's record writer sets this for a quiet item; observers skip its toast and count. */
inline constexpr std::uint16_t kQuietChangeFlag = 0x8;
/** The client's record writer sets this for a toastless item; only the toast is skipped. */
inline constexpr std::uint16_t kToastlessChangeFlag = 0x100;

/** @return The policy bits the client's record writer derives from one item's definition. */
[[nodiscard]] inline std::uint16_t
change_flags(const state::build_data::items::Definition& item) noexcept {
    const unsigned flags = kChangeFlags | (item.quietAcquisition ? kQuietChangeFlag : 0U)
                           | (item.toastlessAcquisition ? kToastlessChangeFlag : 0U);
    return static_cast<std::uint16_t>(flags);
}

/**
 * Names every row an exchange credited, so each gain is drawn and repeats accumulate.
 * @param accountObject Encoded account object being upserted, whose ring is still empty.
 * @param changes Rows the exchange credited.
 * @return Null on success, or the reason the ring could not be written.
 */
[[nodiscard]] const char*
write_exchange_changes(middleware::datagen::family4::account::layout::Object& accountObject,
                       std::span<const state::ProfileStackChange> changes) noexcept;

/** Pins feed-referenced item identities to their published character rows. */
[[nodiscard]] bool apply_acquisition_presentation(
    std::span<std::byte> characterBytes,
    const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
    std::span<const queuez::AcquisitionPresentationRow> presentationRows) noexcept;

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
 * Builds the family-two social roster snapshot.
 *
 * Both slots go out together: the row resolves the emblem by reading the link at directory +8
 * and looking the member record up by it, and a full snapshot prunes whatever it does not name.
 * @param scratch Object storage owned by the lock.
 * @param subscription Family id the Client picked.
 * @param objectId Unused. Both slot ids are resolved by the builder.
 * @param reservation Prior payload prefixes that staging must keep.
 * @param prepared Gets both descriptors and the scratch clear extents.
 * @return True when an account is signed in and both objects fit.
 */
[[nodiscard]] bool prepare_social_roster(Scratch& scratch,
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
 * @param accountSelectionCleared The peer's account object names no character; the character
 *        object stays resident.
 * @param prepared Gets the compressed object descriptors and scratch clear extents.
 * @return True when State, mappings, layouts and the installed compression all fit.
 */
[[nodiscard]] bool
prepare(Scratch& scratch,
        const middleware::queuez::Subscription& subscription,
        std::uint32_t accountObjectId,
        const Reservation& reservation,
        std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
        bool accountSelectionCleared,
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

/**
 * Builds the Family-4 character upsert for one prepared equipment swap.
 * @param scratch Object and compression storage owned by the lock.
 * @param swap Checked queuez version after-image and resident character definition.
 * @param mutation Checked State after-image that is not committed yet.
 * @param acquisitionPresentationRows Item identities pinned to feed-referenced rows.
 * @param prepared Gets the single character upsert descriptor.
 * @return True when the after-image encodes and the complete object fits.
 */
[[nodiscard]] bool prepare_equipment_swap(
    Scratch& scratch,
    const queuez::EquipmentSwap& swap,
    const state::PendingEquipmentSwap& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/** Builds the Family-4 character upsert carrying one accumulated item-state change. */
[[nodiscard]] bool
prepare_item_state(Scratch& scratch,
                   const queuez::EquipmentSwap& update,
                   const state::PendingItemState& mutation,
                   std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
                   Prepared& prepared) noexcept;

/**
 * Builds the selected character's current object as one upsert above the peer version.
 * @param characterIndex The selected character's slot in the account.
 */
[[nodiscard]] bool prepare_selected_character_refresh(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    std::uint64_t accountSoid,
    std::size_t characterIndex,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/** Builds the selected-character upsert for one uncommitted artifact purchase. */
[[nodiscard]] bool prepare_artifact_purchase(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    const state::PendingArtifactPurchase& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/** Builds only the current account and selected-character objects as one upsert. */
[[nodiscard]] bool prepare_account_refresh(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/** Builds only the current account and selected-character objects after an artifact reset. */
[[nodiscard]] bool prepare_artifact_reset(Scratch& scratch,
                                          const queuez::EquipmentSwap& update,
                                          Prepared& prepared) noexcept;

/** Builds one current item-resident upsert after an artifact reset cleared its socket. */
[[nodiscard]] bool prepare_artifact_item_refresh(Scratch& scratch,
                                                 const queuez::EquipmentSwap& update,
                                                 std::uint64_t instanceSoid,
                                                 Prepared& prepared) noexcept;

/**
 * Builds one held item's resident and the selected character's object behind it, after an edit
 * changed the item's own values. The values travel in the resident and the row's new serial in
 * the character object.
 * @param accountRows The edit also changed a row the account object publishes, which then follows.
 */
[[nodiscard]] bool prepare_item_values_refresh(
    Scratch& scratch,
    const queuez::EquipmentSwap& update,
    std::uint64_t instanceSoid,
    bool accountRows,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/**
 * Builds the Family-4 item-instance upsert for one prepared ordinary-socket selection, with the
 * account it charged or paid. The character follows only when a row took a new serial, as the
 * oven's does after a bake; any other selection keeps item identity, placement and serial.
 */
[[nodiscard]] bool prepare_socket_plug(Scratch& scratch,
                                       const queuez::SocketPlug& socketPlug,
                                       const state::PendingSocketPlug& mutation,
                                       Prepared& prepared) noexcept;

/** Builds the Family-4 subclass item-instance upsert for one prepared node selection. */
[[nodiscard]] bool prepare_subclass_selection(Scratch& scratch,
                                              const queuez::SubclassSelection& selection,
                                              const state::PendingSubclassSelection& mutation,
                                              Prepared& prepared) noexcept;

/**
 * Builds one Family-4 increment containing the newly resident item object followed by the
 * changed character that references it.
 * @param scratch Object and compression storage owned by the lock.
 * @param acquisition Exact queuez after-image promised by the correlated response.
 * @param mutation Checked State after-image that remains uncommitted while output is staged.
 * @param prepared Gets the two upsert descriptors in item-then-character dependency order.
 * @return True when both after-image objects encode and fit atomically.
 */
[[nodiscard]] bool prepare_item_acquisition(
    Scratch& scratch,
    const queuez::ItemAcquisition& acquisition,
    const state::PendingItemAcquisition& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/**
 * Builds one Family-4 increment containing the full account after-image for a profile stack.
 *
 * No resident is added: the account root is upserted at the exact staged +1 revision.
 */
[[nodiscard]] bool
prepare_profile_item_acquisition(Scratch& scratch,
                                 const queuez::ProfileItemAcquisition& acquisition,
                                 const state::PendingProfileItemAcquisition& mutation,
                                 Prepared& prepared) noexcept;

/** Builds a transient XP inventory-row acquisition and the account progression after-image. */
[[nodiscard]] bool prepare_seasonal_experience_presentation(
    Scratch& scratch,
    const queuez::SessionState& before,
    std::int32_t amount,
    std::int32_t mutationSerial,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/** Builds one atomic record-reward batch in resident, character, account order. */
[[nodiscard]] bool prepare_record_reward_grant(
    Scratch& scratch,
    const queuez::SessionState& before,
    const queuez::RecordRewardGrant& update,
    const state::PendingRecordRewardGrant& mutation,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    Prepared& prepared) noexcept;

/**
 * Builds one Family-4 increment containing the changed character and released item instance.
 * A stack discard releases nothing, and a changed profile adds the account object.
 * @param scratch Object and compression storage owned by the lock.
 * @param dismantle Exact queuez after-image promised by the correlated response.
 * @param mutation Checked State after-image that remains uncommitted while output is staged.
 * @param prepared Gets the character upsert followed by any empty release descriptor.
 * @return True when the character after-image and the whole update fit atomically.
 */
[[nodiscard]] bool prepare_item_dismantle(Scratch& scratch,
                                          const queuez::ItemDismantle& dismantle,
                                          const state::PendingItemDismantle& mutation,
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
 * Reads the account-object fields State holds outside the account copy, for the account encoder.
 * @param account Account view the object is encoded from.
 * @param fields Receives each character's published value and the Store wallet sync generation.
 * @return False when the saved generation cannot be read.
 */
[[nodiscard]] bool
read_account_state_fields(const state::AccountState& account,
                          middleware::datagen::family4::account::StateFields& fields) noexcept;

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

/** Resolves one source-backed profile stack into the shared Family-4 item-instance schema. */
[[nodiscard]] bool resolve_profile_item_instance(
    const state::account::inventory::ProfileItem& profileItem,
    middleware::datagen::family4::instance::ResolvedInstance& output) noexcept;

/**
 * Appends every source-backed profile item after all character-owned item residents.
 * Native currency, material and consumable rows have zero SOIDs and add no descriptor.
 */
[[nodiscard]] bool append_profile_items(Scratch& scratch,
                                        std::span<std::byte> rawStorage,
                                        std::uint32_t itemInstanceObjectId,
                                        const state::AccountState& account,
                                        std::size_t baseIndex,
                                        Prepared& staged,
                                        std::size_t& itemCursor,
                                        std::size_t& compressedExtent) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
