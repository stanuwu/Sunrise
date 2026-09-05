#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "../../Sunrise/src/core/settings/settings_upgrade.h"
#include "../../Sunrise/src/core/settings/version.h"

namespace {

namespace settings = sunrise::core::settings;

/** Fixed output storage exceeds every synthetic document used here. */
constexpr std::size_t kOutputCapacity = 256;

/**
 * Checks migration text, an exact-fit buffer, and rejection of a short buffer.
 * @param document Synthetic settings input, with no game or account data.
 * @param expected Complete expected JSON, including preserved whitespace.
 * @return True when every output and capacity check passes.
 */
[[nodiscard]] bool check_upgrade(std::string_view document, std::string_view expected) noexcept {
    std::array<char, kOutputCapacity> output{};
    std::size_t written = 0;
    if (!settings::upgrade::apply(document, "{}", output, written)
        || std::string_view(output.data(), written) != expected) {
        std::fprintf(stderr,
                     "Unexpected migration for: %.*s\n",
                     static_cast<int>(document.size()),
                     document.data());
        return false;
    }
    if (!settings::upgrade::apply(document, "{}", std::span(output).first(expected.size()), written)
        || written != expected.size() || std::string_view(output.data(), written) != expected) {
        std::fputs("Exact-fit migration failed\n", stderr);
        return false;
    }
    if (settings::upgrade::apply(
            document, "{}", std::span(output).first(expected.size() - 1), written)) {
        std::fputs("Short migration buffer was accepted\n", stderr);
        return false;
    }
    if (settings::upgrade::needed(expected)) {
        std::fputs("Migrated settings still need an upgrade\n", stderr);
        return false;
    }
    return true;
}

} // namespace

/** @return Success when empty roots and existing members survive version insertion. */
int main() {
    const std::string member = "\"version\": " + std::to_string(settings::kSettingsVersion);
    bool passed = true;
    passed = check_upgrade("{}", "{" + member + "}") && passed;
    passed = check_upgrade("{ }", "{" + member + " }") && passed;
    passed = check_upgrade("{\t\r\n }", "{" + member + "\t\r\n }") && passed;
    passed = check_upgrade(" \n{\r\n}\t", " \n{" + member + "\r\n}\t") && passed;
    passed = check_upgrade("{\"core\":{}}", "{" + member + ",\"core\":{}}") && passed;
    passed = check_upgrade("{ \n\"core\":{} }", "{" + member + ", \n\"core\":{} }") && passed;
    passed = check_upgrade("{\"note\":\"}\"}", "{" + member + ",\"note\":\"}\"}") && passed;
    passed = check_upgrade("{\"version\": 0}", "{" + member + "}") && passed;
    passed = check_upgrade("{" + member + "}", "{" + member + "}") && passed;
    if (!passed) {
        return EXIT_FAILURE;
    }
    std::puts("All 9 settings upgrade cases passed (including exact-fit and short buffers).");
    return EXIT_SUCCESS;
}
