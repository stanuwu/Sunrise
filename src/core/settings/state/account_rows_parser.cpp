#include <limits>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Character levels are stored in one unsigned byte in authored State. */
constexpr std::uint64_t kMaximumCharacterLevel = (std::numeric_limits<std::uint8_t>::max)();
/** A destination definition hash is one unsigned 32-bit field. */
constexpr std::uint64_t kMaximumDestinationHash = (std::numeric_limits<std::uint32_t>::max)();

} // namespace

/** Parses the authored account-wide item array. */
bool Parser::profile_items(state::AccountState& output) noexcept {
    namespace inventory = state::account::inventory;
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

/** Reads one selectable ability's socket entry. */
bool Parser::ability_entry(std::uint8_t& output) noexcept {
    std::uint64_t value = 0;
    if (!unsigned_integer(value) || value > state::kMaximumMovementAbilityEntry) {
        return false;
    }
    output = static_cast<std::uint8_t>(value);
    return true;
}

/** Parses one authored character identity. */
bool Parser::character(state::CharacterState& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    bool hasSoid = false;
    bool hasEquipment = false;
    if (consume('}')) {
        return false;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "soid") {
            if (!unsigned_value(output.soid) || output.soid == 0) {
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
        } else if (key == "movement_ability") {
            if (!ability_entry(output.movementAbilityEntry)) {
                return false;
            }
        } else if (key == "grenade_ability") {
            if (!ability_entry(output.grenadeAbilityEntry)) {
                return false;
            }
        } else if (key == "super_ability") {
            if (!ability_entry(output.superAbilityEntry)) {
                return false;
            }
        } else if (key == "melee_ability") {
            if (!ability_entry(output.meleeAbilityEntry)) {
                return false;
            }
        } else if (key == "class_ability") {
            if (!ability_entry(output.classAbilityEntry)) {
                return false;
            }
        } else if (key == "equipment") {
            if (hasEquipment || !equipment(output.equipment)) {
                return false;
            }
            hasEquipment = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return hasSoid;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
