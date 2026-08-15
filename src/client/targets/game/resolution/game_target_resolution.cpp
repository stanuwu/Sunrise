#include "game_target_resolution.h"

#include <array>

#include "internal.h"

namespace sunrise::client::targets::game::resolution {
namespace {

Failure g_lastFailure{Failure::none};
std::string_view g_lastFailedSignature{};

/** Clears the content commit marker before invalidating any target table. */
void clear_groups() noexcept {
    config_getter::clear();
    packages::clear();
    assert_handler::clear();
    retail_log::clear();
    content::clear();
    network::clear();
}

/**
 * Records the rejecting stage and restores the cleared failure state.
 * @param failure Stage that rejected the resolve.
 * @return False for the aggregate resolver.
 */
[[nodiscard]] bool fail_resolution(Failure failure) noexcept {
    g_lastFailure = failure;
    clear_groups();
    return false;
}

/**
 * Finds the first signature that did not match exactly once.
 * @param definitions Registered signatures in slot order.
 * @param matches Aggregate resolution results.
 * @return Name of the first non-unique signature, or an empty view when all matched.
 */
[[nodiscard]] std::string_view first_unresolved(std::span<const patterns::Pattern> definitions,
                                                std::span<const patterns::Match> matches) noexcept {
    for (std::size_t index = 0; index < matches.size(); ++index) {
        if (matches[index].status != patterns::MatchStatus::unique) {
            return index < definitions.size() ? definitions[index].name : std::string_view{};
        }
    }
    return {};
}

} // namespace

/**
 * Resolves every game target in one image sweep and publishes both groups together.
 * @param image Executable ranges from the main game image.
 * @return True when every signature and derived target validates.
 */
bool resolve(std::span<const patterns::ImageRange> image) noexcept {
    clear_groups();
    g_lastFailure = Failure::none;
    g_lastFailedSignature = {};
    const std::span<const patterns::Pattern> definitions = patterns::game::definitions();
    std::array<patterns::Match, kMatchCount> matches{};
    if (definitions.size() != matches.size()
        || !patterns::resolve_all(image, definitions, matches)) {
        return fail_resolution(Failure::signature);
    }
    const std::span<const patterns::Pattern> required = definitions.first(kRequiredMatchCount);
    g_lastFailedSignature = first_unresolved(
        required, std::span<const patterns::Match>(matches).first(kRequiredMatchCount));
    if (!g_lastFailedSignature.empty()) {
        return fail_resolution(Failure::signature);
    }

    network::Targets networkTargets;
    content::Targets contentTargets;
    const std::span<const patterns::Match> resolvedMatches = matches;
    // A rejection here is a derived address or page check, not a signature miss.
    if (!network::derive(image, resolvedMatches.first(kNetworkMatchCount), networkTargets)) {
        return fail_resolution(Failure::networkDerive);
    }
    if (!content::derive(image,
                         resolvedMatches.subspan(kContentFirstMatch, kContentMatchCount),
                         contentTargets)) {
        return fail_resolution(Failure::contentDerive);
    }

    network::publish(networkTargets);
    // Content readiness is the commit marker after the network table is visible.
    content::publish(contentTargets);
    // Diagnostic only: a miss here leaves capture off and never fails activation.
    retail_log::Targets retailLogTargets;
    if (retail_log::derive(resolvedMatches.subspan(kRetailLogFirstMatch), retailLogTargets)) {
        retail_log::publish(retailLogTargets);
    }
    // Also diagnostic: without it an assert halts the boot behind the game's own dialog.
    assert_handler::Targets assertTargets;
    if (assert_handler::derive(image, assertTargets)) {
        assert_handler::publish(assertTargets);
    }
    packages::Targets packageTargets;
    if (packages::derive(image, packageTargets)) {
        packages::publish(packageTargets);
    }
    config_getter::Targets configGetterTargets;
    if (config_getter::derive(image, configGetterTargets)) {
        config_getter::publish(configGetterTargets);
    }
    return true;
}

/** @return Stage that rejected the last resolve call. */
Failure last_failure() noexcept {
    return g_lastFailure;
}

/** @return Name of the first signature that did not match once, or an empty view. */
std::string_view last_failed_signature() noexcept {
    return g_lastFailedSignature;
}

} // namespace sunrise::client::targets::game::resolution
