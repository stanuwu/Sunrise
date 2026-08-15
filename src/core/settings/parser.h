#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "settings.h"

namespace sunrise::core::settings::parser {

/** Fixed-storage JSON reader for the supported Core settings. */
class Parser {
public:
    /** @param input Complete JSON text, borrowed and never changed. */
    explicit Parser(std::string_view input) noexcept;

    /**
     * Parses the supported root object and skips unknown top-level values.
     * @param output Receives supported settings on top of the caller's defaults.
     * @return True when the input matches the supported root settings.
     */
    [[nodiscard]] bool parse_root(Settings& output) noexcept;

    /**
     * Parses the supported Core settings object.
     * @param output Receives supported Core values.
     * @return True when the object is valid JSON.
     */
private:
    [[nodiscard]] bool core(Settings& output) noexcept;
    /**
     * Parses Client settings on top of the fixed defaults.
     * @param output Receives the complete Client settings after checking.
     * @return True when every supported Client object appears at most once.
     */
    [[nodiscard]] bool client_settings(client::Settings& output) noexcept;
    /**
     * Parses the in-game UI boot and input policy.
     * @param output Receives settings only after the whole object is valid.
     * @return True when supported values are unique and use a named Windows key.
     */
    [[nodiscard]] bool client_ui_settings(ui::runtime::Settings& output) noexcept;
    /**
     * Parses the external-server block on top of the fixed defaults.
     * @param output Receives the settings only after every field is valid.
     * @return True when every supported key appears at most once with a usable value.
     */
    [[nodiscard]] bool client_external_settings(client::external::Settings& output) noexcept;
    /**
     * Checks the standalone Server settings object.
     * @param output Receives the current Server settings after the whole object is valid.
     * @return True when the object is valid JSON; unknown future options are skipped.
     */
    [[nodiscard]] bool server_settings(server::Settings& output) noexcept;
    /**
     * Parses the authored entitlement array. Array order is the handle order the Client finds
     * definitions by, so entries are kept exactly as configured.
     * @param output Receives the whole authored table, replacing the bundled policy.
     * @return True when every entry parses and the array fits the table size.
     */
    [[nodiscard]] bool entitlements(state::entitlements::Table& output) noexcept;
    /**
     * Parses one authored entitlement definition.
     * @param output Receives one filled-in definition.
     * @return True when the object carries a bounded name and a supported ownership form.
     */
    [[nodiscard]] bool entitlement(state::entitlements::Entitlement& output) noexcept;
    /**
     * Parses Steam settings on top of the fixed defaults.
     * @param output Receives the complete Steam settings after checking.
     * @return True when every supported Steam object appears at most once.
     */
    [[nodiscard]] bool steam_settings(steam::Settings& output) noexcept;
    /**
     * Parses the single local Steam user's fixed identity settings.
     * @param output Receives the user settings only after the whole object is valid.
     * @return True when the supported persona is unique and bounded.
     */
    [[nodiscard]] bool steam_user_settings(steam::User& output) noexcept;
    [[nodiscard]] bool state_settings(Settings& output) noexcept;
    /**
     * Parses the optional activity settings object on top of the State defaults.
     * @param output Receives one replacement only after its whole row is valid.
     * @return True when the object is valid JSON with no repeated supported fields.
     */
    [[nodiscard]] bool
    activity_settings(state::activity::defaults::ActivityDefaults& output) noexcept;
    /**
     * Parses one local destination and its small numeric launch policy.
     * @param output Receives the candidate only after all supported fields are valid together.
     * @return True when every required field appears once with a canonical value.
     */
    [[nodiscard]] bool
    default_destination(state::activity::defaults::DefaultDestination& output) noexcept;
    /**
     * Parses one authored arrival override row.
     * @param output Row receiving the candidate only after every supplied field is valid.
     * @return True when the object names a destination and at least one value.
     */
    [[nodiscard]] bool
    arrival_override(state::activity::defaults::ArrivalOverride& output) noexcept;
    /**
     * Parses the authored arrival override table.
     * @param output Defaults receiving the rows and their count.
     * @return True when every row is valid and the table fits fixed storage.
     */
    [[nodiscard]] bool
    arrival_overrides(state::activity::defaults::ActivityDefaults& output) noexcept;
    [[nodiscard]] bool unlocks(state::unlocks::Table& output) noexcept;
    [[nodiscard]] bool flag_runs(std::span<std::uint8_t> bank) noexcept;
    [[nodiscard]] bool flag_indices(std::span<std::uint8_t> bank) noexcept;
    [[nodiscard]] bool objective_values(std::span<std::int32_t> bank) noexcept;
    /**
     * Parses the optional authored family-5 override group.
     * @param output Receives both override lists; id and gate fields stay default.
     * @return True when the object is valid JSON and every row fits its wire bounds.
     */
    [[nodiscard]] bool investment(state::Family5State& output) noexcept;
    /**
     * Fills the flag-override list from [slot, value] pairs.
     * @param output Receives the flag rows and their count.
     * @return True when every slot, value, and the row count fit their bounds.
     */
    [[nodiscard]] bool unlock_flag_overrides(state::Family5State& output) noexcept;
    /**
     * Fills the signed value-override list from [slot, value] pairs.
     * @param output Receives the value rows and their count.
     * @return True when every slot and the row count fit their bounds.
     */
    [[nodiscard]] bool unlock_value_overrides(state::Family5State& output) noexcept;
    [[nodiscard]] bool account(state::AccountState& output) noexcept;
    /**
     * Parses the authored character array.
     * @param output Receives characters in configuration order.
     * @return True when every entry is valid and fits the playable size.
     */
    [[nodiscard]] bool characters(state::AccountState& output) noexcept;
    /**
     * Parses the authored account-wide item array.
     * A profile item names only its definition and quantity. Its slot is not authored: the
     * inventory bucket the definition belongs to names the first slot of that bucket's run.
     * @param output Receives profile items in configuration order.
     * @return True when every entry is complete and the array fits its size.
     */
    [[nodiscard]] bool profile_items(state::AccountState& output) noexcept;
    /**
     * Parses one authored character identity.
     * @param output Receives one complete authored character row.
     * @return True when the object contains one nonzero SOID.
     */
    [[nodiscard]] bool character(state::CharacterState& output) noexcept;

    /**
     * Reads one selectable ability's socket entry.
     * @param output Receives the entry only when it is inside the socket-entry bound.
     * @return True when the value parses and names a possible entry.
     */
    [[nodiscard]] bool ability_entry(std::uint8_t& output) noexcept;
    /**
     * Parses the optional equipment object with its fixed named slots.
     * @param output Receives present items only after the whole object is valid.
     * @return True when every name is known and appears at most once.
     */
    [[nodiscard]] bool equipment(state::account::inventory::Equipment& output) noexcept;
    /**
     * Parses one item only when every required named field appears exactly once.
     * @param output Receives the complete item after checking.
     * @return True when all fields fit their fixed State form.
     */
    [[nodiscard]] bool equipment_item(state::account::inventory::Item& output) noexcept;
    /**
     * Parses the native-default policy or an authored plug-lane array.
     * @param output Receives the canonical socket policy after the value is valid.
     * @return True for null or an array of at most 12 hash-or-null lanes.
     */
    [[nodiscard]] bool equipment_plugs(state::account::inventory::Sockets& output) noexcept;
    /**
     * Parses one unsigned definition hash used by authored inventory.
     * @param output Receives any 32-bit hash except the engine no-definition sentinel.
     * @return True when the integer or quoted hex value is allowed.
     */
    [[nodiscard]] bool inventory_definition_hash(std::uint32_t& output) noexcept;
    /**
     * Parses grouped account settings. Native record offsets stay hidden.
     * @param output Receives the complete authored account settings.
     * @return True when every supplied group is valid and the migration latch is complete.
     */
    [[nodiscard]] bool account_settings(state::account::settings::AccountSettings& output) noexcept;
    /**
     * Parses controller and mouse settings under stable Sunrise-owned names.
     * @param output Receives bounded native value types, with no record offsets.
     * @return True when the whole controls object is valid JSON.
     */
    [[nodiscard]] bool controls_settings(state::account::settings::Controls& output) noexcept;
    /**
     * Parses voice, volume, and migration settings under stable Sunrise-owned names.
     * @param output Receives bounded native value types, with no record offsets.
     * @return True when the whole audio object is valid JSON.
     */
    [[nodiscard]] bool audio_settings(state::account::settings::Audio& output) noexcept;
    /**
     * Parses screen and renderer settings under stable Sunrise-owned names.
     * @param output Receives bounded native value types, with no record offsets.
     * @return True when the whole display object is valid JSON.
     */
    [[nodiscard]] bool display_settings(state::account::settings::Display& output) noexcept;
    /**
     * Parses HUD and text settings under stable Sunrise-owned names.
     * @param output Receives bounded native value types, with no record offsets.
     * @return True when the whole interface object is valid JSON.
     */
    [[nodiscard]] bool interface_settings(state::account::settings::Interface& output) noexcept;
    /**
     * Parses matchmaking and chat settings under stable Sunrise-owned names.
     * @param output Receives bounded native value types, with no record offsets.
     * @return True when the whole social object is valid JSON.
     */
    [[nodiscard]] bool social_settings(state::account::settings::Social& output) noexcept;
    /**
     * Parses the whole fixed action table under named JSON properties.
     * @param output Receives primary and secondary inputs for every supported action.
     * @return True when all actions appear exactly once with both input halves.
     */
    [[nodiscard]] bool
    key_bindings(state::account::settings::bindings::KeyBindings& output) noexcept;
    /**
     * Parses one binding with explicit primary and secondary halves.
     * @param output Receives optional input codes; null means unbound.
     * @return True when both halves appear exactly once.
     */
    [[nodiscard]] bool key_binding(state::account::settings::bindings::Binding& output) noexcept;
    /**
     * Parses one optional input name. Numbers are not accepted.
     * @param output Receives a code or the unbound null state.
     * @return True for null or a name the Client's own input table carries.
     */
    [[nodiscard]] bool optional_input_code(std::optional<std::uint16_t>& output) noexcept;
    /**
     * Turns one authored input name into its input code.
     * @param name Key name, or one modifier and the key it prefixes joined by "+".
     * @return True when every part of the name is in the Client's input table.
     */
    [[nodiscard]] static bool input_code_value(std::string_view name,
                                               std::uint16_t& output) noexcept;
    /**
     * Parses logging sinks and channel levels.
     * @param output Receives supported logging values.
     * @return True when the logging object is valid JSON.
     */
    [[nodiscard]] bool logging(log::Settings& output) noexcept;
    /**
     * Parses named channel levels and ignores unknown channels.
     * @param output Receives the known channel levels.
     * @return True when the levels object is valid JSON.
     */
    [[nodiscard]] bool levels(log::Settings& output) noexcept;
    /**
     * Checks and skips one JSON value of any type without allocating.
     * @param depth Current object or array nesting depth.
     * @return True when one complete value is read within the depth limit.
     */
    [[nodiscard]] bool skip_value(unsigned depth) noexcept;
    /**
     * Reads one JSON string and returns the borrowed encoded bytes.
     * @param output Receives the bytes between the quotes.
     * @return True when quotes, escapes, and control bytes are valid.
     */
    [[nodiscard]] bool string(std::string_view& output) noexcept;
    /**
     * Reads a JSON true or false literal.
     * @return True when either literal is read.
     */
    [[nodiscard]] bool boolean(bool& output) noexcept;
    /**
     * Reads one unsigned decimal integer with no precision loss.
     * @param output Receives the parsed 64-bit value.
     * @return True when a complete integer token fits the target type.
     */
    [[nodiscard]] bool unsigned_integer(std::uint64_t& output) noexcept;
    /**
     * Reads an unsigned id as a JSON integer or a quoted hex token.
     * @param output Receives the parsed 64-bit value.
     * @return True when the whole value fits with no precision loss.
     */
    [[nodiscard]] bool unsigned_value(std::uint64_t& output) noexcept;
    /**
     * Reads one signed JSON integer. Fractions and exponents are refused.
     * @param output Receives the parsed 64-bit value.
     * @return True when the whole integer token fits the target type.
     */
    [[nodiscard]] bool signed_integer(std::int64_t& output) noexcept;
    /**
     * Reads one signed JSON integer into an 8-bit record value.
     * @return True when the integer fits the native signed byte.
     */
    [[nodiscard]] bool signed_byte(std::int8_t& output) noexcept;
    /**
     * Reads one signed JSON integer into a 32-bit record value.
     * @return True when the integer fits the native signed 32-bit value.
     */
    [[nodiscard]] bool signed_32(std::int32_t& output) noexcept;
    /**
     * Reads one finite JSON number into an authored float State value.
     * @return True when the whole token is finite and fits a float.
     */
    [[nodiscard]] bool floating_point(float& output) noexcept;
    /** @return True when one complete JSON number is read. */
    [[nodiscard]] bool number() noexcept;
    /**
     * Reads one exact JSON literal after leading whitespace.
     * @param value Literal bytes to match.
     * @return True when the whole literal is read.
     */
    [[nodiscard]] bool literal(std::string_view value) noexcept;
    /**
     * Reads one expected structural character after whitespace.
     * @return True when the character is read.
     */
    [[nodiscard]] bool consume(char value) noexcept;
    /** @return True when only trailing whitespace remains. */
    [[nodiscard]] bool at_end() noexcept;
    /** Skips JSON whitespace. Other control bytes are left alone. */
    void whitespace() noexcept;
    /**
     * Turns a JSON level token into the logging enum.
     * @param name Borrowed level token.
     * @return True when the token names a supported level.
     */
    [[nodiscard]] static bool level_value(std::string_view name, log::Level& output) noexcept;
    /**
     * Turns a JSON channel token into its settings-array index.
     * @param name Borrowed channel token.
     * @return Channel index, or Channel::count for an unknown token.
     */
    [[nodiscard]] static std::size_t channel_index(std::string_view name) noexcept;
    /**
     * Maps one readable setting name to a Windows SDK virtual key.
     * @param name Lowercase settings key name.
     * @return True when the name is in the supported menu-key set.
     */
    [[nodiscard]] static bool ui_toggle_key_value(std::string_view name, UINT& output) noexcept;

    std::string_view input_;
    std::size_t position_{};
};

} // namespace sunrise::core::settings::parser
