#include <bitset>
#include <limits>
#include <optional>

#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace authored_inventory = state::account::inventory;

/** Required item fields use one presence bit each. */
enum class ItemField : std::size_t {
    instanceSoid,
    definitionHash,
    level,
    quantity,
    plugs,
    count,
};

} // namespace

/** Parses the optional equipment object with its fixed named slots. */
bool Parser::equipment(authored_inventory::Equipment& output) noexcept {
    authored_inventory::Equipment parsed{};
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        output = parsed;
        return true;
    }

    std::bitset<authored_inventory::kEquipmentSlotCount> supplied;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        const std::optional<authored_inventory::EquipmentSlot> slot =
            authored_inventory::slot_from_name(key);
        if (!slot.has_value()) {
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(*slot);
        if (supplied.test(index)) {
            return false;
        }
        supplied.set(index);

        if (literal("null")) {
            parsed.slots[index].reset();
        } else {
            authored_inventory::Item item{};
            if (!equipment_item(item)) {
                return false;
            }
            parsed.slots[index] = item;
        }

        if (consume('}')) {
            if (!authored_inventory::valid(parsed)) {
                return false;
            }
            output = parsed;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one item only when every required named field appears exactly once. */
bool Parser::equipment_item(authored_inventory::Item& output) noexcept {
    authored_inventory::Item parsed{};
    if (!consume('{')) {
        return false;
    }
    std::bitset<static_cast<std::size_t>(ItemField::count)> supplied;
    const auto mark = [&supplied](ItemField field) noexcept {
        const std::size_t index = static_cast<std::size_t>(field);
        if (supplied.test(index)) {
            return false;
        }
        supplied.set(index);
        return true;
    };
    if (consume('}')) {
        return false;
    }

    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "instance_soid") {
            if (!mark(ItemField::instanceSoid) || !unsigned_value(parsed.instanceSoid)
                || parsed.instanceSoid == 0) {
                return false;
            }
        } else if (key == "definition_hash") {
            if (!mark(ItemField::definitionHash)
                || !inventory_definition_hash(parsed.definitionHash)) {
                return false;
            }
        } else if (key == "level") {
            if (!mark(ItemField::level) || !signed_32(parsed.level)) {
                return false;
            }
        } else if (key == "quantity") {
            if (!mark(ItemField::quantity) || !signed_32(parsed.quantity)) {
                return false;
            }
        } else if (key == "plugs") {
            if (!mark(ItemField::plugs) || !equipment_plugs(parsed.sockets)) {
                return false;
            }
        } else {
            return false;
        }

        if (consume('}')) {
            if (!supplied.all() || !authored_inventory::valid(parsed)) {
                return false;
            }
            output = parsed;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the native-default policy or an authored plug-lane array. */
bool Parser::equipment_plugs(authored_inventory::Sockets& output) noexcept {
    authored_inventory::Sockets parsed{};
    if (literal("null")) {
        output = parsed;
        return true;
    }
    if (!consume('[')) {
        return false;
    }
    parsed.policy = authored_inventory::SocketPolicy::authored;
    if (consume(']')) {
        output = parsed;
        return true;
    }

    for (;;) {
        if (parsed.plugCount >= parsed.plugs.size()) {
            return false;
        }
        std::optional<std::uint32_t>& plug = parsed.plugs[parsed.plugCount];
        if (literal("null")) {
            plug.reset();
        } else {
            std::uint32_t definitionHash = 0;
            if (!inventory_definition_hash(definitionHash)) {
                return false;
            }
            plug = definitionHash;
        }
        ++parsed.plugCount;

        if (consume(']')) {
            if (!authored_inventory::valid(parsed)) {
                return false;
            }
            output = parsed;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one unsigned definition hash used by authored inventory. */
bool Parser::inventory_definition_hash(std::uint32_t& output) noexcept {
    std::uint64_t value = 0;
    if (!unsigned_value(value) || value > (std::numeric_limits<std::uint32_t>::max)()
        || value == authored_inventory::kNoDefinitionHash) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

} // namespace sunrise::core::settings::parser
