/**
 * Family-two social roster snapshot: the directory and the member record it links to.
 * The panel resolves the emblem through both objects, and a full snapshot prunes every object it
 * does not name, so the pair goes out in one message.
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/presence/presence_record_encoder.h"
#include "../../../../../state/account/inventory/inventory_state.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/fireteam.h"
#include "../../../../../state/activity/member_presence.h"
#include "../../../../../state/build_data/items/item_catalog.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

/** One line carries the soid, the object count, the encoded size and the resolved member row. */
constexpr std::size_t kReportCapacity = 224;

/**
 * Absent definition index. The variant field always takes it, because a real value there indexes
 * a table the client then walks with a bogus entry.
 */
constexpr std::uint16_t kEmptyDefinitionIndex = 0xFFFFU;

/**
 * Resolves the selected character's equipped emblem to a native definition index.
 * The client reads this object as the account's emblem, so it must track the live loadout.
 * @param account Account snapshot, already read under the lock by the caller.
 * @param index Receives the native definition index of the equipped emblem.
 * @param definitionHash Receives the equipped emblem hash.
 * @return False when nothing is selected, the emblem slot is empty, or the hash is unknown.
 */
[[nodiscard]] bool selected_emblem_definition_index(const state::AccountState& account,
                                                    std::uint16_t& index,
                                                    std::uint32_t& definitionHash) noexcept {
    for (const state::CharacterState& character : account.characters) {
        if (!character.selected) {
            continue;
        }
        const auto& slot =
            character.equipment
                .slots[static_cast<std::size_t>(state::account::inventory::EquipmentSlot::emblem)];
        if (!slot.has_value()) {
            return false;
        }
        state::build_data::items::Definition definition{};
        if (!state::build_data::items::find_hash(slot->definitionHash, definition)) {
            return false;
        }
        index = definition.definitionIndex;
        definitionHash = slot->definitionHash;
        return true;
    }
    return false;
}

/** Zero is the record's own fill, so an unresolved fireteam writes nothing. */
constexpr std::uint32_t kNoGroupKey = 0;
/** All-one bits are the native no-group value and must not be minted from a fold. */
constexpr std::uint32_t kGroupKeySentinelNone = 0xFFFFFFFFU;

/** FNV-1a over one 64-bit value, seeded so several values can be folded in sequence. */
[[nodiscard]] std::uint32_t fold(std::uint32_t seed, std::uint64_t value) noexcept {
    std::uint32_t key = seed;
    for (std::size_t index = 0; index < sizeof value; ++index) {
        key ^= static_cast<std::uint32_t>((value >> (index * 8)) & 0xFFU);
        key *= 16777619U;
    }
    return key;
}

/**
 * Folds one fireteam's representative account into the shared roster group key.
 * Every member of an established fireteam folds the same representative, so the panel groups
 * them under one key even when no client publishes a native one.
 */
[[nodiscard]] std::uint32_t folded_group_key(std::uint64_t representative) noexcept {
    const auto key = fold(2166136261U, representative);
    // Both members land here together, so the substitute is still one shared key.
    return key == kNoGroupKey || key == kGroupKeySentinelNone ? 1U : key;
}

/** The member record's fireteam and activity fields, resolved from server State. */
struct MemberFacts final {
    std::int16_t activityIndex{-1};
    std::int16_t previousActivityIndex{-1};
    std::uint32_t groupKey{};
    std::int8_t memberCount{};
};

/**
 * Resolves the fireteam and activity fields one account root publishes.
 * The account's own native publication owns the key when it has one; otherwise the established
 * fireteam supplies it, which is what gives two players who publish none a shared key. The seat
 * names the live activity: a follower who never sent a launch request has nothing else.
 * @param account Account snapshot already read under the profile lock.
 * @param characterSoid Selected character the member record is about.
 * @param facts Cleared, then filled with whatever resolves.
 */
void resolve_member_facts(const state::AccountState& account,
                          std::uint64_t characterSoid,
                          MemberFacts& facts) noexcept {
    facts = {};
    if (account.presence.native.hasGroup
        && account.presence.native.characterSoid == characterSoid) {
        facts.groupKey = account.presence.native.groupKey;
        facts.memberCount = account.presence.native.memberCount;
    } else if (const auto representative =
                   state::activity::fireteam::representative(account.primarySoid);
               representative != 0) {
        facts.groupKey = folded_group_key(representative);
    }
    state::activity::destination::DestinationSelection seat{};
    const auto seatSession =
        state::activity::presence::seat_session(account.primarySoid, characterSoid);
    if (seatSession != 0 && state::activity::destination::snapshot(seatSession, seat)
        && seat.activityIndex >= 0) {
        facts.activityIndex = seat.activityIndex;
        facts.previousActivityIndex = seat.sourceActivityIndex;
    }
}

} // namespace

/** Builds the family-two snapshot carrying the social roster directory and member record. */
bool prepare_social_roster(Scratch& scratch,
                           const middleware::queuez::Subscription& subscription,
                           std::uint32_t objectId,
                           const Reservation& reservation,
                           Prepared& prepared) noexcept {
    // Both slot ids are resolved here, so the caller's single id is not used.
    (void)objectId;
    auto& account = scratch.accountImage;
    const auto handle = state::account_for_subscription_root(subscription.familyRootSoid);
    const state::ScopedAccountView bind(handle);
    if (!state::bound_account_snapshot(account)) {
        return report_failure("social_roster_state");
    }
    if (account.primarySoid == 0 || account.primarySoid != subscription.familyRootSoid
        || reservation.rawWriteOffset > scratch.plaintext.size()) {
        return report_failure("social_roster_state");
    }
    const auto destination = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    // The body is the directory record followed by the member record.
    const auto characterSoid = state::account::selected_character_soid(account);
    const std::size_t kTotal =
        middleware::datagen::kSocialRosterDirectorySize
        + (characterSoid != 0 ? middleware::datagen::kSocialRosterMemberSize : 0);
    if (destination.size() < kTotal) {
        return report_failure("social_roster_storage");
    }

    std::uint16_t emblem = kEmptyDefinitionIndex;
    std::uint32_t emblemHash = 0;
    if (!selected_emblem_definition_index(account, emblem, emblemHash)) {
        emblem = kEmptyDefinitionIndex;
    }
    MemberFacts facts{};
    resolve_member_facts(account, characterSoid, facts);
    middleware::datagen::presence::Member member{};
    member.characterSoid = characterSoid;
    member.emblem = emblem;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        const auto& character = account.characters[i];
        if (character.soid != characterSoid) {
            continue;
        }
        member.level = character.level;
        member.title = character.equippedTitleRecordIndex;
        member.titleKind = static_cast<std::int8_t>(character.gender);
        // The seat is where this account really is. Its own character row only carries what a
        // launch request wrote, so a follower who sent none read In Orbit for everyone.
        if (facts.activityIndex >= 0) {
            member.activityIndex = facts.activityIndex;
            member.previousActivityIndex = facts.previousActivityIndex;
        } else if (character.currentActivityIndex <= (std::numeric_limits<std::int16_t>::max)()) {
            member.activityIndex = static_cast<std::int16_t>(character.currentActivityIndex);
        }
        if (!state::equipment::light::resolution::character_light(account, i, member.light)) {
            return report_failure("social_roster_light");
        }
    }
    member.groupKey = facts.groupKey;
    member.memberCount = facts.memberCount;

    Prepared staged{};
    std::size_t objectCount = 0;
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t rawUsed = 0;

    /**
     * Writes one object and stages it.
     * Both lookups match on the first qword, so the directory leads with the account soid and
     * links its selected character at +8. That character owns the member record and its emblem.
     */
    const auto emit = [&](std::size_t size, std::uint32_t id, bool directory) noexcept {
        if (objectCount >= staged.objects.size()) {
            return false;
        }
        const auto body = destination.subspan(rawUsed, size);
        const bool encoded = directory
                                 ? middleware::datagen::presence::encode_directory(
                                       account.primarySoid, characterSoid, account.presence, body)
                                 : middleware::datagen::presence::encode_member(member, body);
        if (!encoded) {
            return false;
        }
        std::size_t compressedSize = 0;
        if (!compress_object(scratch,
                             body,
                             id,
                             directory ? account.primarySoid : characterSoid,
                             compressedExtent,
                             staged.objects[objectCount],
                             compressedSize)) {
            return false;
        }
        compressedExtent += compressedSize;
        rawUsed += size;
        ++objectCount;
        return true;
    };

    // The directory goes first so a partial land reads as the directory surviving without a member
    // record, rather than as an unexplained miss.
    if (!emit(middleware::datagen::kSocialRosterDirectorySize,
              middleware::datagen::kSocialRosterDirectoryObjectId,
              true)) {
        return report_failure("social_roster_directory");
    }
    if (characterSoid != 0
        && !emit(middleware::datagen::kSocialRosterMemberSize,
                 middleware::datagen::kSocialRosterMemberObjectId,
                 false)) {
        return report_failure("social_roster_member");
    }

    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + rawUsed);
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        subscription.familyType,
        subscription.familyRootSoid,
        kInitialFamilyVersion,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        return report_failure("social_roster_commit");
    }

    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=queuez stage=social_roster result=ok soid=0x%016llX"
                                      " objects=%zu bytes=%zu emblem=%u hash=0x%08X"
                                      " activity=%d prev=%d group=0x%08X",
                                      static_cast<unsigned long long>(account.primarySoid),
                                      objectCount,
                                      rawUsed,
                                      static_cast<unsigned>(emblem),
                                      static_cast<unsigned>(emblemHash),
                                      static_cast<int>(member.activityIndex),
                                      static_cast<int>(member.previousActivityIndex),
                                      member.groupKey);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

std::uint32_t social_roster_revision(Scratch& scratch, std::uint64_t familyRootSoid) noexcept {
    auto& account = scratch.accountImage;
    const auto handle = state::account_for_subscription_root(familyRootSoid);
    const state::ScopedAccountView bind(handle);
    if (!state::bound_account_snapshot(account)) {
        return 0;
    }
    MemberFacts facts{};
    resolve_member_facts(account, state::account::selected_character_soid(account), facts);
    // A pure function of the served values, so it settles by itself and never repushes a body
    // the subscriber already holds.
    std::uint32_t key = fold(2166136261U, facts.groupKey);
    key = fold(key, static_cast<std::uint16_t>(facts.activityIndex));
    key = fold(key, static_cast<std::uint16_t>(facts.previousActivityIndex));
    return fold(key, static_cast<std::uint8_t>(facts.memberCount));
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
