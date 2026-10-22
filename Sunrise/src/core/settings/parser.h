#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../json/reader.h"
#include "settings.h"

namespace sunrise::core::settings::parser {

/**
 * Fixed-storage JSON reader for the supported Core settings.
 * Every parse below writes `output` only once its whole object is valid, and skips unknown keys.
 * A false return leaves `output` as the caller left it.
 */
class Parser : private json::Reader {
public:
    /** @param input Complete JSON text, borrowed and never changed. */
    explicit Parser(std::string_view input) noexcept;

    /** Parses the root object on top of the caller's defaults. */
    [[nodiscard]] bool parse_root(Settings& output) noexcept;

    /** Reads only the root version; a missing version is zero. */
    [[nodiscard]] bool parse_version(std::uint32_t& output, bool* compact = nullptr) noexcept;

    /** @return The settings name of a supported menu toggle key, or empty when it is not one. */
    [[nodiscard]] static std::string_view ui_toggle_key_name(UINT virtualKey) noexcept;

private:
    /** @return True when the next unread byte opens a string, for the compact forms. */
    [[nodiscard]] bool quoted_value() const noexcept;
    /** Host text, its dotted-quad octets and a port, at the widths both endpoints declare. */
    [[nodiscard]] bool
    bap_endpoint(std::array<char, client::server_endpoint::kHostCapacity>& host,
                 std::array<unsigned char, client::server_endpoint::kAddressOctets>& address,
                 std::uint16_t& port) noexcept;
    [[nodiscard]] bool compact_endpoint(client::server_endpoint::Settings& output) noexcept;
    [[nodiscard]] bool compact_persona(steam::User& output) noexcept;
    /** Parses the Core settings object. */
    [[nodiscard]] bool core(Settings& output) noexcept;
    /** Parses the activity SDK generation block. `lua_declarations` must be unique and boolean. */
    [[nodiscard]] bool
    activity_sdk_generation_settings(ActivitySdkGenerationSettings& output) noexcept;
    /** Parses Client settings. Each supported object may appear at most once. */
    [[nodiscard]] bool client_settings(client::Settings& output, bool& endpointConfigured) noexcept;
    /** Parses the in-game UI boot and input policy. Keys must name a Windows key. */
    [[nodiscard]] bool client_ui_settings(ui::runtime::Settings& output) noexcept;
    /** Parses the external-server block. Each supported key may appear at most once. */
    [[nodiscard]] bool client_external_settings(client::external::Settings& output) noexcept;
    /** Parses the shared service settings object. */
    [[nodiscard]] bool server_settings(server::Settings& output) noexcept;
    /** Parses the activation gate block. Every supported key carries a boolean. */
    [[nodiscard]] bool activation_settings(server::activation::Settings& output) noexcept;
    /** Parses the gameplay endpoint block. Topology, addresses, port and reserve must agree. */
    [[nodiscard]] bool gameplay_settings(server::gameplay::Settings& output) noexcept;
    /** Parses Steam settings. Each supported object may appear at most once. */
    [[nodiscard]] bool steam_settings(steam::Settings& output) noexcept;
    /** Parses the single local Steam user. Its persona must be unique and bounded. */
    [[nodiscard]] bool steam_user_settings(steam::User& output) noexcept;
    /** Parses the State settings object. */
    [[nodiscard]] bool state_settings(Settings& output) noexcept;
    /** Parses the activity settings object on top of the State defaults. */
    [[nodiscard]] bool
    activity_settings(state::activity::defaults::ActivityDefaults& output) noexcept;
    /** Parses one local destination and its launch policy. Every required field appears once. */
    [[nodiscard]] bool
    default_destination(state::activity::defaults::DefaultDestination& output) noexcept;
    /** Parses logging sinks and channel levels. */
    [[nodiscard]] bool logging(log::Settings& output) noexcept;
    /** Parses named channel levels and ignores unknown channels. */
    [[nodiscard]] bool levels(log::Settings& output) noexcept;

    /** Turns a level token into the logging enum. */
    [[nodiscard]] static bool level_value(std::string_view name, log::Level& output) noexcept;
    /** @return Channel index for the token, or Channel::count when it names none. */
    [[nodiscard]] static std::size_t channel_index(std::string_view name) noexcept;
    /** Maps one settings key name to a Windows SDK virtual key from the supported menu set. */
    [[nodiscard]] static bool ui_toggle_key_value(std::string_view name, UINT& output) noexcept;

    /** The reader's input, kept because the reader offers no look-ahead. */
    std::string_view text_;
};

} // namespace sunrise::core::settings::parser
