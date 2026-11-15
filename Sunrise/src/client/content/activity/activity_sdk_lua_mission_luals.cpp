// LuaLS declarations of the mission Lua API, written from the table the mission VM dispatches
// from: one class per receiver, one per event view, the program table and the atom shape.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "../../../server/activity/mission/mission_api.h"
#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

namespace api = ::sunrise::server::activity::mission::api;

/** One generated mission table and the LuaLS class of its rows. */
struct RowClass final {
    std::string_view table{};
    std::string_view luaClass{};
};

// Generated mission tables whose rows already have a LuaLS class.
constexpr RowClass kRowClasses[]{
    {"mission.Directive", "SunriseDirective"},
    {"mission.TaskGroup", "SunriseCombatTaskGroup"},
    {"mission.ActorAbility", "SunriseActorAbility"},
    {"mission.ActorTemperament", "SunriseActorTemperament"},
    {"mission.ActorChannel", "SunriseActorChannel"},
    {"mission.states", "SunriseState"},
    {"mission.Slot", "SunriseSlotHandle"},
    {"mission.Squad", "SunriseSquadHandle"},
    {"mission.Scene", "SunriseSceneHandle"},
};

/** @return The LuaLS class of one generated mission table's rows, or empty when it has none. */
[[nodiscard]] std::string_view row_class(std::string_view table) noexcept {
    for (const RowClass& row : kRowClasses) {
        if (row.table == table) {
            return row.luaClass;
        }
    }
    return {};
}

/** @return The LuaLS class of one receiver, or a plain table for a view this table omits. */
[[nodiscard]] std::string_view receiver_class(std::string_view name) noexcept {
    if (name == "event") {
        return "SunriseEvent";
    }
    for (const api::ReceiverSpec& receiver : api::kReceivers) {
        if (receiver.name == name) {
            return receiver.luaClass;
        }
    }
    return "table";
}

/** @return The LuaLS class of one context.sdk collection's entries. */
[[nodiscard]] std::string_view enum_class(std::string_view name) noexcept {
    for (const api::EnumSpec& collection : api::kEnums) {
        if (collection.name == name) {
            return collection.luaClass;
        }
    }
    return "any";
}

[[nodiscard]] std::string lua_type(const api::ValueSpec& value);

/** @return An inline LuaLS table type with the value's array part and named fields. */
[[nodiscard]] std::string inline_table(const api::ValueSpec& value) {
    std::string output = "{";
    const char* separator = "";
    if (!value.element.empty()) {
        output.append("[integer]: ").append(lua_type(value.element.front()));
        separator = ", ";
    }
    for (const api::ValueSpec& field : value.fields) {
        output.append(separator).append(field.name).append(field.optional ? "?: " : ": ");
        output.append(lua_type(field));
        separator = ", ";
    }
    return output.append("}");
}

/** @return The LuaLS type of one value, without its optional marker. */
[[nodiscard]] std::string lua_type(const api::ValueSpec& value) {
    using Kind = api::ValueKind;
    switch (value.kind) {
    case Kind::boolean:
        return "boolean";
    case Kind::integer:
        return value.ref == "EventKind" ? "SunriseEventKind" : "integer";
    case Kind::number:
        return "number";
    case Kind::string:
    case Kind::decimalString:
    case Kind::name:
        return "string";
    case Kind::scalar:
        return "boolean|integer|number|string";
    case Kind::selector:
        return value.handleAccepted ? "string|integer|" + std::string(row_class(value.ref))
                                    : "string|integer";
    case Kind::handle:
        return std::string(receiver_class(value.ref));
    case Kind::enumValue:
        return std::string(enum_class(value.ref));
    case Kind::missionRow: {
        const std::string_view known = row_class(value.ref);
        return !known.empty()         ? std::string(known)
               : value.fields.empty() ? "table"
                                      : inline_table(value);
    }
    case Kind::list: {
        const std::string element = lua_type(value.element.front());
        return element.find('|') == std::string::npos ? element + "[]" : "(" + element + ")[]";
    }
    case Kind::table: {
        if (value.ref == "atom_kinds") {
            return "table<string, string>";
        }
        const std::string_view known = row_class(value.ref);
        return known.empty() ? inline_table(value) : std::string(known);
    }
    case Kind::atom:
        return "SunriseAtom";
    case Kind::function:
        return "function";
    }
    return "any";
}

/** @return The LuaLS type of a read value, with nil when it may be absent. */
[[nodiscard]] std::string read_type(const api::ValueSpec& value) {
    std::string type = lua_type(value);
    return value.optional ? type.append("|nil") : type;
}

/** @return The note a slot method's gate adds: its slot type and whether it is exact. */
[[nodiscard]] std::string gate_note(const api::SlotGate& gate) {
    if (gate.slotType == api::kAnySlotType) {
        return {};
    }
    std::string note = "Slot type " + std::to_string(gate.slotType);
    return note.append(gate.checks.exactJoin ? " (exact)." : ".");
}

/** @return The LuaLS function type of one callable member. */
[[nodiscard]] std::string function_type(std::string_view self, const api::MemberSpec& member) {
    std::string output = "fun(";
    const char* separator = "";
    if (member.colon) {
        output.append("self: ").append(self);
        separator = ", ";
    }
    if (member.call == api::CallForm::named) {
        bool required = false;
        api::ValueSpec table{.kind = api::ValueKind::table, .fields = member.arguments};
        for (const api::ValueSpec& argument : member.arguments) {
            required = required || !argument.optional;
        }
        output.append(separator)
            .append(required ? "args: " : "args?: ")
            .append(inline_table(table));
    } else {
        for (const api::ValueSpec& argument : member.arguments) {
            output.append(separator).append(argument.name).append(argument.optional ? "?: " : ": ");
            output.append(lua_type(argument));
            separator = ", ";
        }
    }
    output.append(")");
    separator = ": ";
    for (const api::ValueSpec& value : member.returns) {
        output.append(separator).append(read_type(value));
        separator = ", ";
    }
    return output;
}

/** Appends one ---@field line, with the note after the type when there is one. */
void append_field(std::string& output,
                  std::string_view name,
                  std::string_view type,
                  std::string_view note) {
    output.append("---@field ").append(name).append(" ").append(type);
    if (!note.empty()) {
        output.append(" ").append(note);
    }
    output.push_back('\n');
}

/** Appends every member of one list; overloads of one name become one union field. */
void append_members(std::string& output,
                    std::string_view self,
                    std::span<const api::MemberSpec> members) {
    for (std::size_t index = 0; index < members.size(); ++index) {
        const api::MemberSpec& member = members[index];
        if (member.kind == api::MemberKind::field) {
            const api::ValueSpec& type = member.returns.front();
            append_field(output,
                         member.name,
                         read_type(type),
                         member.note.empty() ? type.note : member.note);
            continue;
        }
        bool repeated = false;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            repeated = repeated || members[earlier].name == member.name;
        }
        if (repeated) {
            continue;
        }
        std::string type{};
        std::string note{};
        std::size_t overloads = 0;
        for (std::size_t later = index; later < members.size(); ++later) {
            const api::MemberSpec& overload = members[later];
            if (overload.name != member.name) {
                continue;
            }
            // One signature is written bare; several become a union of parenthesized ones.
            const std::string signature = function_type(self, overload);
            ++overloads;
            if (overloads == 1) {
                type = signature;
            } else {
                if (overloads == 2) {
                    type.insert(0, "(").append(")");
                }
                type.append("|(").append(signature).append(")");
            }
            std::string part = gate_note(overload.gate);
            if (!overload.note.empty()) {
                part.append(part.empty() ? "" : " ").append(overload.note);
            }
            note.append(note.empty() || part.empty() ? "" : " Or: ").append(part);
        }
        append_field(output, member.name, type, note);
    }
}

/** @return A program callback type; an empty event class means no event parameter. */
[[nodiscard]] std::string handler_type(std::string_view event) {
    std::string output = "fun(context: SunriseContext, state: SunriseStateView";
    if (!event.empty()) {
        output.append(", event: ").append(event);
    }
    return output.append(")");
}

/** Appends the EventKind enum, the event classes and the program table. */
void append_events(std::string& output) {
    output.append("---@enum SunriseEventKind\nlocal EventKind = {\n");
    for (const api::EventSpec& event : api::kEvents) {
        output.append("    ").append(event.screaming).append(" = ");
        output.append(std::to_string(event.kind)).append(",\n");
    }
    output.append("}\n\n---@class SunriseEvent\n");
    const api::FieldGroup* const common = api::group(api::FieldGroupId::common);
    append_members(output, "SunriseEvent", common->members);
    output.append("\n---@alias SunriseEventHandler ")
        .append(handler_type("SunriseEvent"))
        .append("\n");
    std::set<std::string_view> emitted{};
    for (const api::EventSpec& event : api::kEvents) {
        if (!emitted.insert(event.luaClass).second) {
            continue;
        }
        output.append("\n---@class ").append(event.luaClass).append(": SunriseEvent\n");
        for (const api::FieldGroup* group : event.groups) {
            if (group != common) {
                append_members(output, event.luaClass, group->members);
            }
        }
    }
    output.append("\n---@class SunriseProgram\n");
    for (const api::ValueSpec& entry : api::kProgramEntries) {
        const std::string type =
            entry.kind == api::ValueKind::function ? handler_type({}) : lua_type(entry);
        append_field(output, std::string(entry.name) + "?", type, entry.note);
    }
    for (const api::EventSpec& event : api::kEvents) {
        append_field(output, std::string(event.callback) + "?", handler_type(event.luaClass), {});
    }
}

/** Appends the atom table shape: every field any atom kind reads, with the kinds that read it. */
void append_atom(std::string& output) {
    std::map<std::string_view, std::string> kinds{};
    std::map<std::string_view, std::string> types{};
    for (const api::AtomKindSpec& kind : api::kAtomKinds) {
        for (const api::ValueSpec& field : kind.fields) {
            std::string& readers = kinds[field.name];
            readers.append(readers.empty() ? "read by " : ", ").append(kind.name);
            // One name may carry a different type per kind, so the field takes their union.
            const std::string type = lua_type(field);
            std::string& known = types[field.name];
            if (("|" + known + "|").find("|" + type + "|") == std::string::npos) {
                known.append(known.empty() ? "" : "|").append(type);
            }
        }
    }
    output.append("\n---@class SunriseAtom\n");
    for (const auto& [name, type] : types) {
        const bool always = name == api::kAtomKindField.name;
        append_field(output,
                     always ? std::string(name) : std::string(name) + "?",
                     type,
                     always ? std::string_view("an atom_kinds name") : kinds[name]);
    }
}

} // namespace

/** Writes the declarations; every receiver class lists exactly the members the VM serves. */
void append_mission_api_luals(std::string& output) {
    append_events(output);
    for (const api::ReceiverSpec& receiver : api::kReceivers) {
        output.append("\n---@class ").append(receiver.luaClass).append("\n");
        append_members(output, receiver.luaClass, receiver.members);
        for (const api::EnumSpec& collection : api::kEnums) {
            if (collection.name != receiver.entries) {
                continue;
            }
            for (const api::EnumEntry& row : collection.entries) {
                append_field(output, row.name, collection.luaClass, {});
            }
        }
        if (receiver.id == api::Receiver::modConfig) {
            append_field(output, "[string]", "integer|number|boolean|nil", receiver.dynamicKeys);
        }
    }
    append_atom(output);
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
