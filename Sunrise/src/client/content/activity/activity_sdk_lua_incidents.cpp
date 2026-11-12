#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

/** Appends one text as a single-line comment body; control characters become spaces. */
void append_comment(std::string& output, std::string_view value) {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        output.push_back(byte < 0x20U || byte == 0x7FU ? ' ' : character);
    }
}

} // namespace

/**
 * Renders sunrise/incidents.lua: every published incident notification, named from its English
 * text. Rows arrive in target order, so a repeated text keeps its bare name on the lowest target
 * and takes _2, _3, ... on the later ones, the same way for every build of one install.
 */
bool render_incident_module(const Source& source, std::string& output) noexcept {
    try {
        std::string names =
            "-- Generated incident notifications. Do not edit.\n"
            "-- Every SObject notification row (type code 0) that has an English text, in target\n"
            "-- order. Send one with context:send_incident{target = incident.Notification.NAME};\n"
            "-- leaving out the payload sends the empty incident header, which is what makes a\n"
            "-- notification show its text. Names come from the English text, and a repeated\n"
            "-- text takes _2, _3, ... in target order.\n"
            "local incident = {}\n\n"
            "---@enum SunriseIncidentNotification\n"
            "incident.Notification = {\n";
        std::string texts = "}\n\n"
                            "--- English text of every notification, by target.\n"
                            "---@type table<integer, string>\n"
                            "incident.Text = {\n";
        std::unordered_set<std::string> used{};
        used.reserve(source.incidentDefinitions.size());
        for (const format::IncidentDefinition& row : source.incidentDefinitions) {
            const std::string_view value = text(source, row.text);
            if (value.empty()) {
                return false;
            }
            const std::string base = identifier(value, true);
            std::string name = base;
            for (std::uint32_t copy = 2; used.contains(name); ++copy) {
                name = base;
                name.push_back('_');
                append_uint(name, copy);
            }
            used.insert(name);
            names += "    ";
            names += name;
            names += " = ";
            append_uint(names, row.target);
            names += ", -- ";
            append_comment(names, value);
            names.push_back('\n');
            texts += "    [";
            append_uint(texts, row.target);
            texts += "] = ";
            append_string(texts, value);
            texts += ",\n";
        }
        output = std::move(names);
        output += texts;
        output += "}\n\nreturn incident\n";
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
