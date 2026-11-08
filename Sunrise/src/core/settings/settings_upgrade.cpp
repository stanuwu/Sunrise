#include "settings_upgrade.h"

#include <cstddef>
#include <utility>

#include "../json/reader.h"
#include "settings.h"

namespace sunrise::core::settings::upgrade {
namespace {

/** Byte range of one key or value in the settings text. */
struct Span final {
    std::size_t begin{};
    std::size_t end{};
};

/** One member found by Locator: the quoted key and its value. */
struct Member final {
    bool found{};
    Span key{};
    Span value{};
};

/** Finds members by name in the root object or in one object below it. */
class Locator final : private json::Reader {
public:
    explicit Locator(std::string_view input) noexcept : Reader(input), input_(input) {}

    /**
     * Finds `key` in the root object, or in the root member `parent` when that is not empty.
     * @param output Receives the spans; `found` stays false when the member is absent.
     * @return False when the text is not a JSON object.
     */
    [[nodiscard]] bool
    find(std::string_view parent, std::string_view key, Member& output) noexcept {
        output = {};
        if (parent.empty()) {
            return members(key, output);
        }
        Member holder{};
        return members(parent, holder, &key, &output);
    }

private:
    /**
     * Walks one object's members. With `inner`, the member named `name` is itself walked for
     * `inner`; otherwise the member named `name` is reported.
     */
    [[nodiscard]] bool members(std::string_view name,
                               Member& output,
                               const std::string_view* inner = nullptr,
                               Member* innerOutput = nullptr) noexcept {
        if (!consume('{')) {
            return false;
        }
        if (consume('}')) {
            return true;
        }
        for (;;) {
            whitespace();
            const std::size_t keyBegin = position();
            std::string_view key;
            if (!string(key)) {
                return false;
            }
            const std::size_t keyEnd = position();
            if (!consume(':')) {
                return false;
            }
            whitespace();
            const std::size_t valueBegin = position();
            if (key == name && inner != nullptr) {
                if (!members(*inner, *innerOutput)) {
                    return false;
                }
            } else if (!skip_value(1)) {
                return false;
            }
            if (key == name) {
                output = {true, {keyBegin, keyEnd}, {valueBegin, trimmed_end(position())}};
            }
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    /** @return `end` moved back over trailing JSON whitespace. */
    [[nodiscard]] std::size_t trimmed_end(std::size_t end) const noexcept {
        while (end > 0
               && (input_[end - 1] == ' ' || input_[end - 1] == '\t' || input_[end - 1] == '\n'
                   || input_[end - 1] == '\r')) {
            --end;
        }
        return end;
    }

    std::string_view input_;
};

/** @return The spaces and tabs before `at` on its line, or nothing when other text precedes it. */
[[nodiscard]] std::string_view line_indent(std::string_view text, std::size_t at) noexcept {
    std::size_t start = at;
    while (start > 0 && (text[start - 1] == ' ' || text[start - 1] == '\t')) {
        --start;
    }
    if (start == 0 || text[start - 1] != '\n') {
        return {};
    }
    return text.substr(start, at - start);
}

/** Replaces `span` in `text`; the caller replaces later spans first. */
void replace(std::string& text, Span span, std::string_view value) {
    text.replace(span.begin, span.end - span.begin, value);
}

/** 18 -> 19: `client.dump_package_keys` moves to `client.exports.package_keys`. */
[[nodiscard]] bool from_18(std::string_view document, std::string& output) {
    // This step writes version 19; the next step starts from it.
    constexpr std::string_view kWrittenVersion = "19";
    Member version{};
    Member keys{};
    if (!Locator(document).find({}, "version", version) || !version.found
        || !Locator(document).find("client", "dump_package_keys", keys)) {
        return false;
    }
    output.assign(document);
    // Later span first, so the earlier span's offsets still hold.
    const bool keysFirst = keys.found && keys.key.begin < version.value.begin;
    if (keysFirst) {
        replace(output, version.value, kWrittenVersion);
    }
    if (keys.found) {
        const std::string_view value =
            document.substr(keys.value.begin, keys.value.end - keys.value.begin);
        const std::string_view indent = line_indent(document, keys.key.begin);
        std::string exports;
        if (indent.empty()) {
            exports.append(R"("exports": {"package_keys": )")
                .append(value)
                .append(R"(, "tag_definitions": false})");
        } else {
            // A member on its own line keeps the file's two-space nesting.
            const std::string nested = std::string("\n").append(indent).append("  ");
            exports.append(R"("exports": {)")
                .append(nested)
                .append(R"("package_keys": )")
                .append(value)
                .append(",")
                .append(nested)
                .append(R"("tag_definitions": false)")
                .append("\n")
                .append(indent)
                .append("}");
        }
        replace(output, {keys.key.begin, keys.value.end}, exports);
    }
    if (!keysFirst) {
        replace(output, version.value, kWrittenVersion);
    }
    return true;
}

} // namespace

/** Runs each upgrade step from `version` up to the current version, in order. */
bool to_current(std::string_view document, std::uint32_t version, std::string& output) noexcept {
    if (version < kOldestUpgradable || version >= kSettingsVersion) {
        return false;
    }
    try {
        std::string current(document);
        for (std::uint32_t step = version; step < kSettingsVersion; ++step) {
            std::string next;
            bool upgraded = false;
            switch (step) {
            case 18:
                upgraded = from_18(current, next);
                break;
            default:
                break;
            }
            if (!upgraded) {
                return false;
            }
            current = std::move(next);
        }
        output = std::move(current);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::core::settings::upgrade
