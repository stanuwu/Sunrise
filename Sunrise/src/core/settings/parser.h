#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "settings.h"

namespace sunrise::core::settings::parser {

/**
 * Fixed-storage JSON reader for the supported Core settings.
 * Every parse below writes `output` only once its whole object is valid, and skips unknown keys.
 * A false return leaves `output` as the caller left it.
 */
class Parser {
public:
    /** @param input Complete JSON text, borrowed and never changed. */
    explicit Parser(std::string_view input) noexcept;

    /** Parses the root object on top of the caller's defaults. */
    [[nodiscard]] bool parse_root(Settings& output) noexcept;

    /** Reads only the root version; a missing version is zero. */
    [[nodiscard]] bool parse_version(std::uint32_t& output, bool* compact = nullptr) noexcept;

private:
    /** Host text, its dotted-quad octets and a port, at the widths both endpoints declare. */
    [[nodiscard]] bool
    bap_endpoint(std::array<char, client::server_endpoint::kHostCapacity>& host,
                 std::array<unsigned char, client::server_endpoint::kAddressOctets>& address,
                 std::uint16_t& port) noexcept;
    [[nodiscard]] bool compact_endpoint(client::server_endpoint::Settings& output) noexcept;
    [[nodiscard]] bool compact_persona(steam::User& output) noexcept;
    /** Parses the Core settings object. */
    [[nodiscard]] bool core(Settings& output) noexcept;
    /** Parses the activity SDK generation boot gate. `enabled` must be unique and boolean. */
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

    // --- JSON scanning ---------------------------------------------------------------------
    // These read one token each and never allocate.

    /** Checks and skips one value of any type. @param depth Current nesting depth. */
    [[nodiscard]] bool skip_value(unsigned depth) noexcept;
    /** Reads one string. @param output Receives the borrowed bytes between the quotes. */
    [[nodiscard]] bool string(std::string_view& output) noexcept;
    /** Reads a true or false literal. */
    [[nodiscard]] bool boolean(bool& output) noexcept;
    /** Reads one unsigned decimal integer with no precision loss. */
    [[nodiscard]] bool unsigned_integer(std::uint64_t& output) noexcept;
    /** Reads an unsigned id as an integer or a quoted hex token. */
    [[nodiscard]] bool unsigned_value(std::uint64_t& output) noexcept;
    /** Reads one signed integer. Fractions and exponents are refused. */
    [[nodiscard]] bool signed_integer(std::int64_t& output) noexcept;
    /** Reads one signed integer that has to fit a native signed byte. */
    [[nodiscard]] bool signed_byte(std::int8_t& output) noexcept;
    /** Reads one signed integer that has to fit a native signed 32-bit value. */
    [[nodiscard]] bool signed_32(std::int32_t& output) noexcept;
    /** Reads one finite number that has to fit a float. */
    [[nodiscard]] bool floating_point(float& output) noexcept;
    /** Reads one complete number. */
    [[nodiscard]] bool number() noexcept;
    /** Reads one exact literal after leading whitespace. @param value Literal bytes to match. */
    [[nodiscard]] bool literal(std::string_view value) noexcept;
    /** Reads one expected structural character after whitespace. */
    [[nodiscard]] bool consume(char value) noexcept;
    /** @return True when only trailing whitespace remains. */
    [[nodiscard]] bool at_end() noexcept;
    /** Skips JSON whitespace. Other control bytes are left alone. */
    void whitespace() noexcept;

    /** Turns a level token into the logging enum. */
    [[nodiscard]] static bool level_value(std::string_view name, log::Level& output) noexcept;
    /** @return Channel index for the token, or Channel::count when it names none. */
    [[nodiscard]] static std::size_t channel_index(std::string_view name) noexcept;
    /** Maps one settings key name to a Windows SDK virtual key from the supported menu set. */
    [[nodiscard]] static bool ui_toggle_key_value(std::string_view name, UINT& output) noexcept;

    std::string_view input_;
    std::size_t position_{};
};

} // namespace sunrise::core::settings::parser
