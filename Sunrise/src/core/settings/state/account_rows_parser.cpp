#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include "../../../state/build_data/items/item_catalog.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Character levels are stored in one unsigned byte in authored State. */
constexpr std::uint64_t kMaximumCharacterLevel = (std::numeric_limits<std::uint8_t>::max)();
/** A destination definition hash is one unsigned 32-bit field. */
constexpr std::uint64_t kMaximumDestinationHash = (std::numeric_limits<std::uint32_t>::max)();

[[nodiscard]] int hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + value - 'a';
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + value - 'A';
    }
    return -1;
}

/** Decodes one exact fixed-width opaque creator block stored on an authored character. */
[[nodiscard]] bool decode_hex_bytes(std::string_view text, std::span<std::byte> output) noexcept {
    if (text.size() != output.size() * 2U) {
        return false;
    }
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = hex_digit(text[index * 2U]);
        const int low = hex_digit(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

/** Sets the tier bit one rarity name stands for. */
[[nodiscard]] bool dismantle_tier_bit(std::string_view name, std::uint8_t& mask) noexcept {
    using Tier = state::build_data::items::Tier;
    Tier tier = Tier::none;
    if (name == "common") {
        tier = Tier::common;
    } else if (name == "uncommon") {
        tier = Tier::uncommon;
    } else if (name == "rare") {
        tier = Tier::rare;
    } else if (name == "legendary") {
        tier = Tier::legendary;
    } else if (name == "exotic") {
        tier = Tier::exotic;
    } else {
        return false;
    }
    const std::uint8_t bit = static_cast<std::uint8_t>(1U << static_cast<unsigned>(tier));
    if ((mask & bit) != 0) {
        return false;
    }
    mask |= bit;
    return true;
}

} // namespace

/** Parses the materials credited by ordinary gear dismantles, with optional rarity/class filters.
 */
bool Parser::dismantle_rewards(state::AccountState& output) noexcept {
    output.dismantleRewards = {};
    output.dismantleRewardCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.dismantleRewardCount >= output.dismantleRewards.size() || !consume('{')) {
            return false;
        }
        state::DismantleRewardPolicy reward{};
        bool hasHash = false;
        bool hasQuantity = false;
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return false;
            }
            std::uint64_t value = 0;
            if (key == "definition_hash") {
                if (hasHash || !unsigned_value(value) || value == 0
                    || value > (std::numeric_limits<std::uint32_t>::max)()) {
                    return false;
                }
                reward.definitionHash = static_cast<std::uint32_t>(value);
                hasHash = true;
            } else if (key == "quantity") {
                if (hasQuantity || !unsigned_integer(value) || value == 0
                    || value > (std::numeric_limits<std::int32_t>::max)()) {
                    return false;
                }
                reward.quantity = static_cast<std::int32_t>(value);
                hasQuantity = true;
            } else if (key == "rarity") {
                // One name or an array of names; each sets its tier bit.
                if (reward.tierMask != 0) {
                    return false;
                }
                const bool list = consume('[');
                for (;;) {
                    std::string_view name;
                    if (!string(name) || !dismantle_tier_bit(name, reward.tierMask)) {
                        return false;
                    }
                    if (!list || consume(']')) {
                        break;
                    }
                    if (!consume(',')) {
                        return false;
                    }
                }
            } else if (key == "class") {
                std::string_view name;
                if (reward.classMask != 0 || !string(name)) {
                    return false;
                }
                if (name == "weapon") {
                    reward.classMask = static_cast<std::uint8_t>(state::DismantleGearClass::weapon);
                } else if (name == "armor") {
                    reward.classMask = static_cast<std::uint8_t>(state::DismantleGearClass::armor);
                } else {
                    return false;
                }
            } else if (key == "masterworked") {
                bool masterworked = false;
                if (reward.masterwork != state::DismantleMasterworkFilter::any
                    || !boolean(masterworked)) {
                    return false;
                }
                reward.masterwork = masterworked
                                        ? state::DismantleMasterworkFilter::masterworked
                                        : state::DismantleMasterworkFilter::notMasterworked;
            } else if (!skip_value(0)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
        }
        for (std::size_t index = 0; index < output.dismantleRewardCount; ++index) {
            if (state::same_dismantle_policy_key(output.dismantleRewards[index], reward)) {
                return false;
            }
        }
        if (!hasHash || !hasQuantity) {
            return false;
        }
        output.dismantleRewards[output.dismantleRewardCount++] = reward;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the authored account-wide item array. */
bool Parser::profile_items(state::AccountState& output) noexcept {
    namespace inventory = state::account::inventory;
    output.profileItems = {};
    output.profileItemCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.profileItemCount >= output.profileItems.size() || !consume('{')) {
            return false;
        }
        inventory::ProfileItem item{};
        bool hasHash = false;
        bool hasQuantity = false;
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return false;
            }
            std::uint64_t value = 0;
            if (key == "definition_hash") {
                if (hasHash || !unsigned_value(value)
                    || value > (std::numeric_limits<std::uint32_t>::max)()) {
                    return false;
                }
                item.definitionHash = static_cast<std::uint32_t>(value);
                hasHash = true;
            } else if (key == "quantity") {
                if (hasQuantity || !unsigned_integer(value)
                    || value > (std::numeric_limits<std::int32_t>::max)() || value == 0) {
                    return false;
                }
                item.quantity = static_cast<std::int32_t>(value);
                hasQuantity = true;
            } else if (!skip_value(0)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
        }
        if (!hasHash || !hasQuantity) {
            return false;
        }
        output.profileItems[output.profileItemCount++] = item;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the authored character array. */
bool Parser::characters(state::AccountState& output) noexcept {
    output.characters = {};
    output.characterCount = 0;
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.characterCount >= output.characters.size()
            || !character(output.characters[output.characterCount])) {
            return false;
        }
        ++output.characterCount;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one authored character identity, including optional native creator presentation data. */
bool Parser::character(state::CharacterState& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    bool hasSoid = false;
    bool hasEquipment = false;
    bool hasInventory = false;
    bool hasPresentation = false;
    bool hasCreationHeader = false;
    bool hasCreationTail = false;
    bool hasCreatorTrailer = false;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "soid") {
            if (hasSoid || !unsigned_value(output.soid) || output.soid == 0) {
                return false;
            }
            hasSoid = true;
        } else if (key == "race") {
            std::uint64_t value = 0;
            if (!unsigned_integer(value)
                || value > static_cast<std::uint8_t>(state::CharacterRace::exo)) {
                return false;
            }
            output.race = static_cast<state::CharacterRace>(value);
        } else if (key == "gender") {
            std::uint64_t value = 0;
            if (!unsigned_integer(value)
                || value > static_cast<std::uint8_t>(state::CharacterGender::female)) {
                return false;
            }
            output.gender = static_cast<state::CharacterGender>(value);
        } else if (key == "class") {
            std::uint64_t value = 0;
            if (!unsigned_integer(value)
                || value > static_cast<std::uint8_t>(state::CharacterClass::warlock)) {
                return false;
            }
            output.characterClass = static_cast<state::CharacterClass>(value);
        } else if (key == "level") {
            std::uint64_t value = 0;
            if (!unsigned_integer(value) || value > kMaximumCharacterLevel) {
                return false;
            }
            output.level = static_cast<std::uint8_t>(value);
        } else if (key == "accepted") {
            if (!boolean(output.accepted)) {
                return false;
            }
        } else if (key == "preview_available") {
            if (!boolean(output.previewAvailable)) {
                return false;
            }
        } else if (key == "appearance_value") {
            if (!floating_point(output.appearanceValue)) {
                return false;
            }
        } else if (key == "last_orbited_destination") {
            std::uint64_t value = 0;
            if (!unsigned_value(value) || value > kMaximumDestinationHash) {
                return false;
            }
            output.lastOrbitedDestination = static_cast<std::uint32_t>(value);
        } else if (key == "content_bypass") {
            if (!boolean(output.contentBypass)) {
                return false;
            }
        } else if (key == "presentation_header") {
            std::string_view value;
            if (hasPresentation || !string(value)
                || !decode_hex_bytes(value, output.presentationHeader)) {
                return false;
            }
            hasPresentation = true;
        } else if (key == "creation_header") {
            std::string_view value;
            if (hasCreationHeader || !string(value)
                || !decode_hex_bytes(value, output.creationHeader)) {
                return false;
            }
            hasCreationHeader = true;
        } else if (key == "creation_tail") {
            std::string_view value;
            if (hasCreationTail || !string(value)
                || !decode_hex_bytes(value, output.creationTail)) {
                return false;
            }
            hasCreationTail = true;
        } else if (key == "creator_trailer") {
            std::uint64_t value = 0;
            if (hasCreatorTrailer || !unsigned_integer(value) || value > 0x1FU) {
                return false;
            }
            output.creatorTrailer = static_cast<std::uint8_t>(value);
            hasCreatorTrailer = true;
        } else if (key == "movement_ability" || key == "grenade_ability" || key == "super_ability"
                   || key == "melee_ability" || key == "class_ability") {
            // Deliberately ignored on load. The subclass screen's first paint each login shows
            // the ability-entry struct defaults below, so restoring a persisted pick would leave
            // that paint disagreeing with what is equipped. Still written out, never read back.
            if (!skip_value(0)) {
                return false;
            }
        } else if (key == "equipment") {
            if (hasEquipment || !equipment(output.equipment)) {
                return false;
            }
            hasEquipment = true;
        } else if (key == "inventory") {
            if (hasInventory || !character_inventory(output.inventory)) {
                return false;
            }
            hasInventory = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            const bool anyCreator = hasPresentation || hasCreationHeader || hasCreationTail
                                    || hasCreatorTrailer;
            const bool completeCreator = hasPresentation && hasCreationHeader && hasCreationTail
                                         && hasCreatorTrailer;
            return hasSoid && (!anyCreator || completeCreator);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
