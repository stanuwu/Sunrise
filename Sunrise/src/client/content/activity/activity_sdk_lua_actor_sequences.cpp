#include <array>
#include <cstdio>
#include <set>

#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

/** Every extracted row remains inspectable; only slot-owned runtime values can be played. */
void append_actor_sequence_contract(const Source& source, std::string& output) {
    output.append(R"lua(
---@class SunriseActorSequence
---@field id string
---@field name string|nil
---@field symbol string
---@field source_path string|nil
---@field key integer
---@field kind integer
---@field resource_tag integer
---@field table_index integer
---@field ordinal integer
---@field source_offset integer
---@field playable boolean

---@class SunriseActorSequences
---@field count integer
---@field at fun(self: SunriseActorSequences, ordinal: integer): SunriseActorSequence|nil
)lua");
    std::set<std::string> symbols{};
    for (const auto& row : source.actorSequenceEntries) {
        const auto symbol = text(source, row.symbol);
        if (!symbol.empty()) {
            symbols.emplace(symbol);
        }
        std::array<char, 13> alias{};
        std::snprintf(alias.data(), alias.size(), "KEY_%08X", row.keyHash);
        symbols.emplace(alias.data());
    }
    for (const auto& symbol : symbols) {
        output.append("---@field ").append(symbol).append(" SunriseActorSequence|nil\n");
    }
    Value::Array tables{};
    Value::Array entries{};
    Value::Array bindings{};
    for (const auto& row : source.actorSequenceTables) {
        tables.push_back(object({{"definition_tag", number(row.definitionTag)},
                                 {"definition_class", number(row.definitionClass)},
                                 {"array_offset", number(row.arrayOffset)},
                                 {"first_entry", number(row.entries.first)},
                                 {"entry_count", number(row.entries.count)},
                                 {"flags", number(row.flags)}}));
    }
    for (const auto& row : source.actorSequenceEntries) {
        entries.push_back(object({{"id", string(text(source, row.id))},
                                  {"name", string(text(source, row.name))},
                                  {"symbol", string(text(source, row.symbol))},
                                  {"source_path", string(text(source, row.sourcePath))},
                                  {"table_index", number(row.tableIndex)},
                                  {"ordinal", number(row.ordinal)},
                                  {"source_offset", number(row.sourceOffset)},
                                  {"key", number(row.keyHash)},
                                  {"kind", number(row.kind)},
                                  {"resource_tag", number(row.resourceTag)},
                                  {"flags", number(row.flags)}}));
    }
    for (const auto& row : source.actorSequenceBindings) {
        bindings.push_back(object({{"actor_class_index", number(row.actorClassIndex)},
                                   {"component_ordinal", number(row.componentOrdinal)},
                                   {"config_tag", number(row.configTag)},
                                   {"source_offset", number(row.sourceOffset)},
                                   {"global_table_index", number(row.globalTableIndex)},
                                   {"local_table_index", number(row.localTableIndex)},
                                   {"source_class", number(row.sourceClass)},
                                   {"flags", number(row.flags)}}));
    }
    std::string catalog{};
    if (!render_lua(object({{"tables", array(std::move(tables))},
                            {"entries", array(std::move(entries))},
                            {"bindings", array(std::move(bindings))}}),
                    0,
                    catalog)) {
        throw std::bad_alloc{};
    }
    output.append("\nsdk.ActorSequenceCatalog = ").append(catalog).append("\n\n");
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
