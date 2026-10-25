#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "activity_sdk_lua_artifacts.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {

/** Small JSON-compatible tree keeps the JSON and Lua declarations identical. */
struct Value final {
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Data =
        std::variant<std::monostate, bool, std::uint64_t, std::int64_t, std::string, Array, Object>;

    Data data{};
};

[[nodiscard]] Value null_value();
[[nodiscard]] Value boolean(bool value);
[[nodiscard]] Value number(std::uint64_t value);
[[nodiscard]] Value signed_number(std::int64_t value);
[[nodiscard]] Value string(std::string_view value);
[[nodiscard]] Value array(Value::Array values);
[[nodiscard]] Value object(Value::Object values);
[[nodiscard]] Value string_array(std::initializer_list<std::string_view> values);

/** One declared field: its name and its contract type. */
using Field = std::pair<std::string_view, std::string_view>;

/** Sorts object members by key. */
void sort_keys(Value::Object& value);

/** @return One object mapping each field name to its type string. */
[[nodiscard]] Value field_map(std::initializer_list<Field> source);

/** Builds one view's declared fields, expanding each prefix into its seven name lanes. */
[[nodiscard]] Value named_view(std::initializer_list<Field> source,
                               std::initializer_list<std::string_view> prefixes,
                               std::string_view rowScope);

/** Builds one immutable view of plain fields that throws on a stale read. */
[[nodiscard]] Value view(std::initializer_list<Field> fields, std::string_view rowScope);

/** Renders one deterministic pretty JSON value without adding a final newline. */
[[nodiscard]] bool
render_json(const Value& value, std::uint32_t initialDepth, std::string& output) noexcept;

/** Renders one deterministic Lua literal without adding a final newline. */
[[nodiscard]] bool
render_lua(const Value& value, std::uint32_t initialDepth, std::string& output) noexcept;

[[nodiscard]] bool build_catalog_sdk_contract_value(Value& output) noexcept;
[[nodiscard]] bool build_manifest_sdk_contract_value(Value& output) noexcept;
void append_world_core_views(Value::Object& output);
void append_world_spatial_views(Value::Object& output);
[[nodiscard]] bool build_world_sdk_contract_value(Value& output) noexcept;
/** Builds the mission Lua API schema published as sunrise/mission_api.json. */
[[nodiscard]] bool build_mission_api_value(Value& output) noexcept;
/** Appends the LuaLS classes of every mission API receiver, event and program entry. */
void append_mission_api_luals(std::string& output);
[[nodiscard]] bool render_activity_files(const Source& source, Bundle& output) noexcept;
[[nodiscard]] bool render_contract_files(const Source& source, Bundle& output) noexcept;
/** Renders sunrise/incidents.lua from the published incident definitions. */
[[nodiscard]] bool render_incident_module(const Source& source, std::string& output) noexcept;
void append_actor_sequence_contract(const Source& source, std::string& output);

[[nodiscard]] std::string_view text(const Source& source, format::StringRef reference) noexcept;
[[nodiscard]] std::string digest_hex(const std::array<std::byte, 32>& digest, bool prefix);

/** Appends one Lua string literal without depending on a runtime serializer. */
void append_string(std::string& output, std::string_view value);
void append_hex(std::string& output, std::uint32_t value);
void append_uint(std::string& output, std::uint32_t value);

/** Converts arbitrary extracted names to stable Lua identifiers or safe file stems. */
[[nodiscard]] std::string identifier(std::string_view value, bool upper);

/** Names one generated module file from its extracted name and stable identity. */
[[nodiscard]] std::string stem(std::string_view name, std::uint32_t identity);

/** @return True when one complete row range lies inside a bank. */
[[nodiscard]] bool range_inside(format::Range range, std::size_t size) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
