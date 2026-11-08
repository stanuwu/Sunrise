#include "mod_manifest_parser.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <span>

#include "../../../core/json/reader.h"
#include "mod_manifest_bindings.h"
#include "mod_version.h"

namespace sunrise::state::mods::manifest {
namespace {

/** Root keys. The Studio writer emits them in this order. */
constexpr std::string_view kManifestVersionKey = "manifestVersion";
constexpr std::string_view kIdKey = "id";
constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kNameKey = "name";
constexpr std::string_view kDescriptionKey = "description";
constexpr std::string_view kAuthorsKey = "authors";
constexpr std::string_view kSourceKey = "src";
constexpr std::string_view kDependenciesKey = "dependencies";
constexpr std::string_view kIncompatibleKey = "incompatible";
constexpr std::string_view kCheatKey = "cheat";
constexpr std::string_view kConfigKey = "config";
/** Startup package bindings require manifest version 3. */
constexpr std::string_view kBindingsKey = "configBindings";
constexpr std::uint32_t kBindingsVersion = 3;
/** The mod's own package ids require manifest version 4. */
constexpr std::string_view kPackagesKey = "packages";
constexpr std::uint32_t kPackagesVersion = 4;
/** Dependency object keys. `id` is shared with the root. */
constexpr std::string_view kMinimumKey = "minVersion";
constexpr std::string_view kMaximumKey = "maxVersion";
/** Config entry keys. `name` is shared with the root. */
constexpr std::string_view kConfigKeyField = "key";
constexpr std::string_view kConfigTypeField = "type";
constexpr std::string_view kConfigDefaultField = "default";
/** Type names a config entry may declare. */
constexpr std::string_view kIntegerType = "int";
constexpr std::string_view kFloatType = "float";
constexpr std::string_view kBooleanType = "bool";
/** The manifest version that added `cheat` and `config`. */
constexpr std::uint32_t kCheatAndConfigVersion = 2;
/** Decoded text below this byte is a control character. */
constexpr unsigned char kFirstPrintable = 0x20;
/** Dots separate id segments. */
constexpr char kIdSeparator = '.';
/** An unknown key is quoted in the message up to this many bytes. */
constexpr int kQuotedKeyLimit = 32;

/** @return True for a character that may start an id segment. */
[[nodiscard]] bool segment_start(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

/** @return True for a character allowed inside an id segment. */
[[nodiscard]] bool segment_character(char value) noexcept {
    return segment_start(value) || value == '-' || value == '_';
}

/** @return True for a character allowed in a config key. */
[[nodiscard]] bool key_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
}

/** @return What a default of `type` must be, for the refusal message. */
[[nodiscard]] const char* type_rule(ConfigType type) noexcept {
    switch (type) {
    case ConfigType::integer:
        return "a whole number that fits 32 bits";
    case ConfigType::floating:
        return "a finite number that fits a float";
    case ConfigType::boolean:
        return "true or false";
    }
    return "";
}

/** Root keys already read, so a repeat is refused. */
struct Seen final {
    bool manifestVersion{};
    bool id{};
    bool version{};
    bool name{};
    bool description{};
    bool authors{};
    bool source{};
    bool dependencies{};
    bool incompatible{};
    bool cheat{};
    bool config{};
    bool bindings{};
    bool packages{};
};

/** Config entry fields already read, so a repeat is refused. */
struct EntrySeen final {
    bool key{};
    bool type{};
    bool defaultValue{};
    bool name{};
};

/** Reads one manifest and writes the first refusal into the caller's error text. */
class Parser final : private core::json::Reader {
public:
    Parser(std::string_view json, Error& error) noexcept
        : Reader(json), json_(json), error_(error) {}

    [[nodiscard]] bool root(Manifest& output) noexcept;
    [[nodiscard]] bool version_only(std::uint32_t& output) noexcept;

private:
    [[nodiscard]] bool fail(const char* format, ...) noexcept;
    [[nodiscard]] bool once(bool& seen, std::string_view key) noexcept;
    [[nodiscard]] bool unknown(std::string_view key) noexcept;
    [[nodiscard]] bool root_key(std::string_view key, Manifest& output, Seen& seen) noexcept;
    template <std::size_t Capacity>
    [[nodiscard]] bool text(std::string_view key, Text<Capacity>& output, bool required) noexcept;
    [[nodiscard]] bool id(std::string_view key, Id& output) noexcept;
    [[nodiscard]] bool version(std::string_view key, Version& output) noexcept;
    [[nodiscard]] bool authors(Manifest& output) noexcept;
    [[nodiscard]] bool dependencies(Manifest& output) noexcept;
    [[nodiscard]] bool dependency(Dependency& output) noexcept;
    [[nodiscard]] bool incompatible(Manifest& output) noexcept;
    [[nodiscard]] bool packages(Manifest& output) noexcept;
    [[nodiscard]] bool config(Manifest& output) noexcept;
    [[nodiscard]] bool config_entry(std::span<const ConfigEntry> earlier,
                                    ConfigEntry& output) noexcept;
    [[nodiscard]] bool entry_field(std::string_view key,
                                   ConfigEntry& output,
                                   EntrySeen& seen,
                                   std::string_view& defaultText) noexcept;
    [[nodiscard]] bool config_key(Text<kConfigKeyCapacity>& output) noexcept;
    [[nodiscard]] bool config_type(ConfigType& output) noexcept;
    [[nodiscard]] bool check_references(const Manifest& output) noexcept;

    std::string_view json_;
    Error& error_;
    /** One-based config entry being read, named in each message; zero outside `config`. */
    std::size_t entry_{};
};

/**
 * Records one refusal with the byte it was found at. Inside `config` the entry is named first.
 * @return Always false, so a caller can return it.
 */
bool Parser::fail(const char* format, ...) noexcept {
    std::size_t length = 0;
    if (entry_ != 0) {
        const int prefix =
            std::snprintf(error_.bytes.data(), error_.bytes.size(), "config entry %zu: ", entry_);
        length =
            prefix > 0 ? (std::min)(static_cast<std::size_t>(prefix), error_.bytes.size() - 1) : 0;
    }
    std::va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(
        error_.bytes.data() + length, error_.bytes.size() - length, format, arguments);
    va_end(arguments);
    length += written > 0 ? static_cast<std::size_t>(written) : 0;
    length = (std::min)(length, error_.bytes.size() - 1);
    const int suffix = std::snprintf(
        error_.bytes.data() + length, error_.bytes.size() - length, " (byte %zu)", position());
    if (suffix > 0) {
        length = (std::min)(length + static_cast<std::size_t>(suffix), error_.bytes.size() - 1);
    }
    error_.length = length;
    return false;
}

/** @return False when `key` was already read. */
bool Parser::once(bool& seen, std::string_view key) noexcept {
    if (seen) {
        return fail("'%s' appears twice", key.data());
    }
    seen = true;
    return true;
}

/** @return Always false, with the raw key quoted. */
bool Parser::unknown(std::string_view key) noexcept {
    return fail("unknown key '%.*s'",
                static_cast<int>((std::min)(key.size(), std::size_t{kQuotedKeyLimit})),
                key.data());
}

/**
 * Reads one string, decodes it, and refuses control characters.
 * @param key Constant key name, for the message.
 * @param output Receives the decoded text.
 * @param required True when an empty value is refused.
 * @return False when the value is not a fitting string.
 */
template <std::size_t Capacity>
bool Parser::text(std::string_view key, Text<Capacity>& output, bool required) noexcept {
    std::string_view raw;
    if (!string(raw)) {
        return fail("'%s' must be a string", key.data());
    }
    if (!core::json::decode_string(raw, output.bytes, output.length)) {
        return fail("'%s' is longer than %zu bytes or has a bad escape", key.data(), Capacity);
    }
    const std::string_view decoded = output.view();
    if (std::any_of(decoded.begin(), decoded.end(), [](char value) {
            return static_cast<unsigned char>(value) < kFirstPrintable;
        })) {
        return fail("'%s' holds a control character", key.data());
    }
    if (required && decoded.empty()) {
        return fail("'%s' is empty", key.data());
    }
    return true;
}

/** Reads one mod id string. */
bool Parser::id(std::string_view key, Id& output) noexcept {
    if (!text(key, output, true)) {
        return false;
    }
    if (!valid_id(output.view())) {
        return fail("'%s' must be a lowercase id such as dev.example.mod", key.data());
    }
    return true;
}

/** Reads one `MAJOR.MINOR.PATCH` string. */
bool Parser::version(std::string_view key, Version& output) noexcept {
    std::string_view raw;
    if (!string(raw) || !version::parse(raw, output)) {
        return fail("'%s' must be MAJOR.MINOR.PATCH, each 0 to 65535", key.data());
    }
    return true;
}

/** Reads the author list. */
bool Parser::authors(Manifest& output) noexcept {
    if (!consume('[')) {
        return fail("'authors' must be a list of strings");
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.authorCount == output.authors.size()) {
            return fail("'authors' lists more than %zu names", output.authors.size());
        }
        if (!text(kAuthorsKey, output.authors[output.authorCount++], true)) {
            return false;
        }
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return fail("'authors' expected ',' or ']'");
        }
    }
}

/**
 * Reads one dependency object. `id` and `minVersion` are required.
 * @return False on an unknown key, a repeat, or a range whose maximum is below its minimum.
 */
bool Parser::dependency(Dependency& output) noexcept {
    if (!consume('{')) {
        return fail("each dependency must be an object");
    }
    bool hasId = false;
    bool hasMinimum = false;
    if (!consume('}')) {
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return fail("dependency expected a key");
            }
            if (key == kIdKey) {
                if (!once(hasId, kIdKey) || !id(kIdKey, output.id)) {
                    return false;
                }
            } else if (key == kMinimumKey) {
                if (!once(hasMinimum, kMinimumKey) || !version(kMinimumKey, output.minimum)) {
                    return false;
                }
            } else if (key == kMaximumKey) {
                if (!once(output.hasMaximum, kMaximumKey)
                    || !version(kMaximumKey, output.maximum)) {
                    return false;
                }
            } else {
                return fail("unknown dependency key '%.*s'",
                            static_cast<int>((std::min)(key.size(), std::size_t{kQuotedKeyLimit})),
                            key.data());
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("dependency expected ',' or '}'");
            }
        }
    }
    if (!hasId || !hasMinimum) {
        return fail("each dependency needs 'id' and 'minVersion'");
    }
    if (output.hasMaximum && version::compare(output.maximum, output.minimum) < 0) {
        return fail("dependency %s has 'maxVersion' below 'minVersion'", output.id.bytes.data());
    }
    return true;
}

/** Reads the dependency list. */
bool Parser::dependencies(Manifest& output) noexcept {
    if (!consume('[')) {
        return fail("'dependencies' must be a list of objects");
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.dependencyCount == output.dependencies.size()) {
            return fail("'dependencies' lists more than %zu mods", output.dependencies.size());
        }
        if (!dependency(output.dependencies[output.dependencyCount++])) {
            return false;
        }
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return fail("'dependencies' expected ',' or ']'");
        }
    }
}

/** Reads the incompatible id list. */
bool Parser::incompatible(Manifest& output) noexcept {
    if (!consume('[')) {
        return fail("'incompatible' must be a list of ids");
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.incompatibleCount == output.incompatible.size()) {
            return fail("'incompatible' lists more than %zu mods", output.incompatible.size());
        }
        if (!id(kIncompatibleKey, output.incompatible[output.incompatibleCount++])) {
            return false;
        }
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return fail("'incompatible' expected ',' or ']'");
        }
    }
}

/** Reads the own package id list, each a "0x" hex string, and keeps it ascending. */
bool Parser::packages(Manifest& output) noexcept {
    if (!consume('[')) {
        return fail("'packages' must be a list of package ids");
    }
    if (!consume(']')) {
        for (;;) {
            if (output.packageCount == output.packages.size()) {
                return fail("'packages' lists more than %zu ids", output.packages.size());
            }
            std::uint64_t value = 0;
            if (!unsigned_value(value) || !own_package_id(value)) {
                return fail("'packages' ids must be 0x%04X to 0x%04X",
                            kFirstOwnPackageId,
                            kLastOwnPackageId);
            }
            output.packages[output.packageCount++] = static_cast<std::uint16_t>(value);
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return fail("'packages' expected ',' or ']'");
            }
        }
    }
    const auto ids = std::span(output.packages).first(output.packageCount);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return fail("'packages' lists an id twice");
    }
    return true;
}

/** Reads one config key and checks its characters. */
bool Parser::config_key(Text<kConfigKeyCapacity>& output) noexcept {
    std::string_view raw;
    if (!string(raw) || !core::json::decode_string(raw, output.bytes, output.length)
        || !valid_config_key(output.view())) {
        return fail("'key' must be 1 to %zu of a-z, 0-9 and _, starting with a letter",
                    kConfigKeyMaximumLength);
    }
    return true;
}

/** Reads one type name: `int`, `float` or `bool`. */
bool Parser::config_type(ConfigType& output) noexcept {
    std::string_view raw;
    if (string(raw) && config_type_name(raw, output)) {
        return true;
    }
    return fail("'type' must be \"int\", \"float\" or \"bool\"");
}

/**
 * Reads the value of one config entry field.
 * @param key Raw field name.
 * @param output Entry receiving the field.
 * @param seen Fields already read.
 * @param defaultText Receives the raw `default` value; it is judged once the type is known.
 * @return False on an unknown field, a repeat, or a bad value.
 */
bool Parser::entry_field(std::string_view key,
                         ConfigEntry& output,
                         EntrySeen& seen,
                         std::string_view& defaultText) noexcept {
    if (key == kConfigKeyField) {
        return once(seen.key, kConfigKeyField) && config_key(output.key);
    }
    if (key == kConfigTypeField) {
        return once(seen.type, kConfigTypeField) && config_type(output.defaultValue.type);
    }
    if (key == kConfigDefaultField) {
        if (!once(seen.defaultValue, kConfigDefaultField)) {
            return false;
        }
        whitespace();
        const std::size_t start = position();
        if (!skip_value(0)) {
            return fail("'default' must be a JSON value");
        }
        defaultText = json_.substr(start, position() - start);
        return true;
    }
    if (key == kNameKey) {
        return once(seen.name, kNameKey) && text(kNameKey, output.name, true);
    }
    return unknown(key);
}

/**
 * Reads one config entry object. `key`, `type` and `default` are required.
 * @param earlier Entries read before this one, so a repeated key is refused.
 * @param output Receives the entry.
 * @return False on a missing, unknown or repeated field, a bad value, or a repeated key.
 */
bool Parser::config_entry(std::span<const ConfigEntry> earlier, ConfigEntry& output) noexcept {
    if (!consume('{')) {
        return fail("must be an object");
    }
    EntrySeen seen{};
    std::string_view defaultText;
    if (!consume('}')) {
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return fail("expected a key");
            }
            if (!entry_field(key, output, seen, defaultText)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or '}'");
            }
        }
    }
    if (!seen.key || !seen.type || !seen.defaultValue) {
        return fail("'key', 'type' and 'default' are required");
    }
    const std::string_view newKey = output.key.view();
    if (std::any_of(earlier.begin(), earlier.end(), [newKey](const ConfigEntry& entry) {
            return entry.key.view() == newKey;
        })) {
        return fail("key '%s' is used twice", output.key.bytes.data());
    }
    if (!read_config_value(defaultText, output.defaultValue)) {
        return fail("'default' must be %s", type_rule(output.defaultValue.type));
    }
    return true;
}

/** Reads the config list. Messages inside it name the entry by its place in the list. */
bool Parser::config(Manifest& output) noexcept {
    if (!consume('[')) {
        return fail("'config' must be a list of objects");
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.configCount == output.config.size()) {
            return fail("'config' lists more than %zu entries", output.config.size());
        }
        entry_ = output.configCount + 1;
        const auto earlier = std::span<const ConfigEntry>(output.config).first(output.configCount);
        if (!config_entry(earlier, output.config[output.configCount])) {
            return false;
        }
        entry_ = 0;
        ++output.configCount;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return fail("'config' expected ',' or ']'");
        }
    }
}

/**
 * Reads the value of one root key.
 * @return False on an unknown key, a repeat, or a bad value.
 */
bool Parser::root_key(std::string_view key, Manifest& output, Seen& seen) noexcept {
    // Messages name the constant key; the raw key is not null-terminated.
    if (key == kManifestVersionKey) {
        if (!once(seen.manifestVersion, kManifestVersionKey)) {
            return false;
        }
        std::uint64_t value = 0;
        if (!unsigned_integer(value) || value > (std::numeric_limits<std::uint32_t>::max)()) {
            return fail("'manifestVersion' must be a whole number");
        }
        output.manifestVersion = static_cast<std::uint32_t>(value);
        return true;
    }
    if (key == kIdKey) {
        return once(seen.id, kIdKey) && id(kIdKey, output.id);
    }
    if (key == kVersionKey) {
        return once(seen.version, kVersionKey) && version(kVersionKey, output.version);
    }
    if (key == kNameKey) {
        return once(seen.name, kNameKey) && text(kNameKey, output.name, true);
    }
    if (key == kDescriptionKey) {
        return once(seen.description, kDescriptionKey)
               && text(kDescriptionKey, output.description, false);
    }
    if (key == kAuthorsKey) {
        return once(seen.authors, kAuthorsKey) && authors(output);
    }
    if (key == kSourceKey) {
        return once(seen.source, kSourceKey) && text(kSourceKey, output.source, false);
    }
    if (key == kDependenciesKey) {
        return once(seen.dependencies, kDependenciesKey) && dependencies(output);
    }
    if (key == kIncompatibleKey) {
        return once(seen.incompatible, kIncompatibleKey) && incompatible(output);
    }
    if (key == kCheatKey) {
        return once(seen.cheat, kCheatKey)
               && (boolean(output.cheat) || fail("'cheat' must be true or false"));
    }
    if (key == kConfigKey) {
        return once(seen.config, kConfigKey) && config(output);
    }
    if (key == kBindingsKey) {
        if (!once(seen.bindings, kBindingsKey)) {
            return false;
        }
        whitespace();
        const std::size_t start = position();
        if (!skip_value(0)) {
            return fail("'configBindings' must be an array");
        }
        return read_bindings(json_.substr(start, position() - start), output, error_);
    }
    if (key == kPackagesKey) {
        return once(seen.packages, kPackagesKey) && packages(output);
    }
    return unknown(key);
}

/**
 * Refuses self references, repeats, and an id that is both required and incompatible.
 * @return False with the first offending id named.
 */
bool Parser::check_references(const Manifest& output) noexcept {
    const std::string_view self = output.id.view();
    for (std::size_t index = 0; index < output.dependencyCount; ++index) {
        const std::string_view dependencyId = output.dependencies[index].id.view();
        if (dependencyId == self) {
            return fail("a mod cannot depend on itself");
        }
        for (std::size_t other = index + 1; other < output.dependencyCount; ++other) {
            if (output.dependencies[other].id.view() == dependencyId) {
                return fail("dependency %s is listed twice",
                            output.dependencies[index].id.bytes.data());
            }
        }
        for (std::size_t other = 0; other < output.incompatibleCount; ++other) {
            if (output.incompatible[other].view() == dependencyId) {
                return fail("%s is both a dependency and incompatible",
                            output.dependencies[index].id.bytes.data());
            }
        }
    }
    for (std::size_t index = 0; index < output.incompatibleCount; ++index) {
        const std::string_view incompatibleId = output.incompatible[index].view();
        if (incompatibleId == self) {
            return fail("a mod cannot be incompatible with itself");
        }
        for (std::size_t other = index + 1; other < output.incompatibleCount; ++other) {
            if (output.incompatible[other].view() == incompatibleId) {
                return fail("incompatible id %s is listed twice",
                            output.incompatible[index].bytes.data());
            }
        }
    }
    return true;
}

/**
 * Reads the whole root object, then checks required keys, the version and references.
 * @param output Empty manifest that receives the fields.
 * @return False with the first refusal recorded.
 */
bool Parser::root(Manifest& output) noexcept {
    Seen seen{};
    if (!consume('{')) {
        return fail("mod.json must hold one JSON object");
    }
    if (!consume('}')) {
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return fail("expected a quoted key");
            }
            if (!root_key(key, output, seen)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return fail("expected ',' or '}'");
            }
        }
    }
    if (!at_end()) {
        return fail("text follows the closing '}'");
    }
    if (!seen.manifestVersion || !seen.id || !seen.version || !seen.name) {
        return fail("'manifestVersion', 'id', 'version' and 'name' are required");
    }
    if (output.manifestVersion < kOldestManifestVersion
        || output.manifestVersion > kManifestVersion) {
        return fail("'manifestVersion' %u is not supported", output.manifestVersion);
    }
    // The version may follow the keys, so a key newer than the version is refused here.
    if (output.manifestVersion < kCheatAndConfigVersion && (seen.cheat || seen.config)) {
        return fail("'%s' needs 'manifestVersion' %u or higher",
                    seen.cheat ? kCheatKey.data() : kConfigKey.data(),
                    kCheatAndConfigVersion);
    }
    if (output.manifestVersion < kBindingsVersion && seen.bindings) {
        return fail("'configBindings' needs 'manifestVersion' %u or higher", kBindingsVersion);
    }
    if (output.manifestVersion < kPackagesVersion && seen.packages) {
        return fail("'packages' needs 'manifestVersion' %u or higher", kPackagesVersion);
    }
    return check_references(output) && check_bindings(output, error_);
}

/**
 * Reads only `manifestVersion` and skips every other value.
 * @return False when the text is not one JSON object.
 */
bool Parser::version_only(std::uint32_t& output) noexcept {
    output = 0;
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return at_end();
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        std::uint64_t value = 0;
        if (key == kManifestVersionKey) {
            if (!unsigned_integer(value) || value > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            output = static_cast<std::uint32_t>(value);
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return at_end();
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace

/** @return True for a lowercase reverse-domain id with at least two segments. */
bool valid_id(std::string_view id) noexcept {
    if (id.size() < kIdMinimumLength || id.size() > kIdMaximumLength) {
        return false;
    }
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t end = (std::min)(id.find(kIdSeparator, start), id.size());
        const std::string_view segment = id.substr(start, end - start);
        if (segment.empty() || !segment_start(segment.front())
            || !std::all_of(segment.begin(), segment.end(), segment_character)) {
            return false;
        }
        ++segments;
        start = end + 1;
    }
    return segments >= 2;
}

/** @return True for a config key: 1 to 32 of `a-z`, `0-9` and `_`, starting with a letter. */
bool valid_config_key(std::string_view key) noexcept {
    return !key.empty() && key.size() <= kConfigKeyMaximumLength && key.front() >= 'a'
           && key.front() <= 'z' && std::all_of(key.begin(), key.end(), key_character);
}

/** Maps `int`, `float` and `bool` to their config type. */
bool config_type_name(std::string_view name, ConfigType& output) noexcept {
    if (name == kIntegerType) {
        output = ConfigType::integer;
        return true;
    }
    if (name == kFloatType) {
        output = ConfigType::floating;
        return true;
    }
    if (name == kBooleanType) {
        output = ConfigType::boolean;
        return true;
    }
    return false;
}

/**
 * Reads one JSON value as a config type.
 * @param json The value text and nothing else.
 * @param output Its `type` picks the reading. Receives the value; changed only on success.
 * @return False when the text is not one value of that type.
 */
bool read_config_value(std::string_view json, ConfigValue& output) noexcept {
    core::json::Reader reader(json);
    ConfigValue parsed = output;
    bool read = false;
    switch (output.type) {
    case ConfigType::integer:
        read = reader.signed_32(parsed.integer);
        break;
    case ConfigType::floating:
        read = reader.floating_point(parsed.floating);
        break;
    case ConfigType::boolean:
        read = reader.boolean(parsed.boolean);
        break;
    }
    if (!read || !reader.at_end()) {
        return false;
    }
    output = parsed;
    return true;
}

/** Reads only the root `manifestVersion`. */
bool read_version(std::string_view json, std::uint32_t& output) noexcept {
    Error ignored{};
    Parser parser(json, ignored);
    return parser.version_only(output);
}

/** Parses one `mod.json` of a supported version. */
bool parse(std::string_view json, Manifest& output, Error& error) noexcept {
    error = {};
    Manifest parsed{};
    Parser parser(json, error);
    if (!parser.root(parsed)) {
        return false;
    }
    output = parsed;
    return true;
}

} // namespace sunrise::state::mods::manifest
