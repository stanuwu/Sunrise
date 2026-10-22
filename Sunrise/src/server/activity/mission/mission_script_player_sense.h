#pragma once

#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Type-13 participation records carry one player each; this is their slot type and root schema. */
inline constexpr std::uint8_t kParticipationSlotType = 13;
inline constexpr std::uint32_t kParticipationSenseSchema = 0x80804F2FU;
/** Object tag that owns the participation records. Assumed the same in every activity. */
inline constexpr std::uint32_t kParticipationObjectTag = 0x80FEB3DCU;
/** Player records occupy sixteen slots starting at slot five. */
inline constexpr std::uint16_t kFirstParticipationSlot = 5;
inline constexpr std::size_t kParticipationSlotCount = 16;
/** The identity block carries the player key in field 0. */
inline constexpr std::uint32_t kParticipationIdentitySchema = 0x808094DDU;
/** The state block carries the region, loaded, settled and Ghost flags. */
inline constexpr std::uint32_t kParticipationStateSchema = 0x808094E4U;
inline constexpr std::uint16_t kParticipationKeyOrdinal = 0;
inline constexpr std::uint16_t kParticipationRegionOrdinal = 0;
inline constexpr std::uint16_t kParticipationLoadedOrdinal = 2;
inline constexpr std::uint16_t kParticipationSettledOrdinal = 4;
inline constexpr std::uint16_t kParticipationGhostOrdinal = 5;
inline constexpr std::uint8_t kLifeSeenKey = 0x01;
inline constexpr std::uint8_t kLifeSeenRegion = 0x02;
inline constexpr std::uint8_t kLifeSeenLoaded = 0x04;
inline constexpr std::uint8_t kLifeSeenSettled = 0x08;
inline constexpr std::uint8_t kLifeSeenGhost = 0x10;
inline constexpr std::uint8_t kLifeSeenAll =
    kLifeSeenKey | kLifeSeenRegion | kLifeSeenLoaded | kLifeSeenSettled | kLifeSeenGhost;
/** All-one bits is not a player key. */
inline constexpr std::uint64_t kInvalidPlayerKey = (std::numeric_limits<std::uint64_t>::max)();
/** The region field is a signed 16-bit slice-set index. */
inline constexpr std::int32_t kMaximumLifeRegion = (std::numeric_limits<std::int16_t>::max)();

enum class PlayerLife : std::uint8_t { unknown, alive, dead };

/** Last participation level seen for one player slot. */
struct PlayerLifeObservation final {
    std::uint64_t playerKey{};
    std::int32_t region{-1};
    std::uint8_t seen{};
    bool loaded{};
    bool settled{};
    bool ghost{};

    /** Dead means loaded, settled and showing the Ghost. Anything incomplete is unknown. */
    [[nodiscard]] PlayerLife life() const noexcept {
        if (seen != kLifeSeenAll || playerKey == 0 || playerKey == kInvalidPlayerKey || region < 0
            || region > kMaximumLifeRegion || !loaded || !settled) {
            return PlayerLife::unknown;
        }
        return ghost ? PlayerLife::dead : PlayerLife::alive;
    }
};

/** State fields a participation report carries besides the key. */
inline constexpr std::uint8_t kLifeSeenState =
    kLifeSeenRegion | kLifeSeenLoaded | kLifeSeenSettled | kLifeSeenGhost;

/**
 * Merges one decoded type-13 delta into the level. An absent field is unchanged; the client
 * sends the region once and never sends the key.
 */
inline void
update_player_life(PlayerLifeObservation& level,
                   std::span<const middleware::bap::activity_message::sense_update::DecodedValue>
                       values) noexcept {
    // A slot can be reused within one connection. Never carry the former player's death
    // evidence across a native identity replacement or withdrawal.
    for (const auto& value : values) {
        if (value.schemaRow == kParticipationIdentitySchema
            && value.fieldOrdinal == kParticipationKeyOrdinal
            && (!value.present || value.unsignedValue != level.playerKey)) {
            level = {};
            break;
        }
    }
    for (const auto& value : values) {
        const bool identity = value.schemaRow == kParticipationIdentitySchema
                              && value.fieldOrdinal == kParticipationKeyOrdinal;
        if (!value.present) {
            continue;
        }
        if (identity) {
            level.playerKey = value.unsignedValue;
            level.seen |= kLifeSeenKey;
        } else if (value.schemaRow == kParticipationStateSchema) {
            switch (value.fieldOrdinal) {
            case kParticipationRegionOrdinal:
                level.region = static_cast<std::int32_t>(value.signedValue);
                level.seen |= kLifeSeenRegion;
                break;
            case kParticipationLoadedOrdinal:
                level.loaded = value.unsignedValue != 0;
                level.seen |= kLifeSeenLoaded;
                break;
            case kParticipationSettledOrdinal:
                level.settled = value.unsignedValue != 0;
                level.seen |= kLifeSeenSettled;
                break;
            case kParticipationGhostOrdinal:
                level.ghost = value.unsignedValue != 0;
                level.seen |= kLifeSeenGhost;
                break;
            default:
                break;
            }
        }
    }
}

/** Life counts over one private activity's joined party. */
struct FireteamLife final {
    std::uint16_t alive{};
    std::uint16_t dead{};
    std::uint16_t unknown{};

    /** An unknown member blocks the all-dead edge, so a loading player cannot cause a wipe. */
    [[nodiscard]] bool all_dead() const noexcept {
        return dead != 0 && alive == 0 && unknown == 0;
    }

    void add(PlayerLife life) noexcept {
        if (life == PlayerLife::alive) {
            ++alive;
        } else if (life == PlayerLife::dead) {
            ++dead;
        } else {
            ++unknown;
        }
    }

    bool operator==(const FireteamLife&) const = default;
};

} // namespace sunrise::server::activity::mission
