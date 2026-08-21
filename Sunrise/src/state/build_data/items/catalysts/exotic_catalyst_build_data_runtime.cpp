#include <Windows.h>

#include "../../runtime.h"
#include "../../runtime/persistence/build_data_persistence.h"
#include "../../runtime/persistence/publication_transaction.h"
#include "exotic_catalyst_builder.h"
#include "exotic_catalyst_catalog.h"

namespace sunrise::state::build_data {
namespace {

/**
 * Binds caller-owned package domains to the active executable identity.
 * @param source Caller-owned installed package domains.
 * @param bound Receives the same domains with the active build identity.
 * @return True when the build-data runtime has an active identity.
 */
[[nodiscard]] bool bind_active_build(const items::catalysts::Source& source,
                                     items::catalysts::Source& bound) noexcept {
    runtime::persistence::Context& state = runtime::persistence::context();
    AcquireSRWLockShared(&state.lock);
    const BuildIdentity build = state.buildIdentity;
    const bool enabled = state.enabled;
    ReleaseSRWLockShared(&state.lock);
    if (!enabled) {
        return false;
    }
    bound = source;
    bound.build = build;
    return true;
}

/**
 * Re-derives one candidate from the same package domains before publication.
 * @param source Caller-owned installed package domains.
 * @param definitions Candidate catalyst rows.
 * @return True when a fresh derivation exactly matches the candidate.
 */
[[nodiscard]] bool
valid_publication(const items::catalysts::Source& source,
                  std::span<const items::catalysts::Definition> definitions) noexcept {
    items::catalysts::Source bound{};
    return bind_active_build(source, bound) && items::catalysts::valid(definitions)
           && items::catalysts::matches_derived(
               bound, items::catalysts::generated_facts(), definitions);
}

/**
 * Records the last whole-catalog derivation result for cache policy.
 * @param error None after a safe derivation, or its exact failure reason.
 */
void record_derivation_error(items::catalysts::Error error) noexcept {
    runtime::persistence::Context& state = runtime::persistence::context();
    AcquireSRWLockExclusive(&state.lock);
    if (state.enabled) {
        state.catalystError = error;
    }
    ReleaseSRWLockExclusive(&state.lock);
}

} // namespace

bool exotic_catalysts_ready() noexcept {
    return items::catalysts::count() != 0;
}

bool derive_exotic_catalysts(const items::catalysts::Source& source,
                             std::span<items::catalysts::Definition> output,
                             std::size_t& count,
                             items::catalysts::Report& report) noexcept {
    items::catalysts::Source bound{};
    if (!bind_active_build(source, bound)) {
        count = 0;
        report = {};
        report.error = items::catalysts::Error::unsupportedBuild;
        report.unsupported = 1;
        record_derivation_error(report.error);
        return false;
    }
    const bool derived =
        items::catalysts::derive(bound, items::catalysts::generated_facts(), output, count, report);
    record_derivation_error(derived ? items::catalysts::Error::none : report.error);
    return derived;
}

bool publish_exotic_catalysts(const items::catalysts::Source& source,
                              std::span<const items::catalysts::Definition> definitions) noexcept {
    if (!valid_publication(source, definitions)) {
        return false;
    }
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return false;
    }
    return transaction.finish(items::catalysts::replace(definitions), items::catalysts::clear);
}

items::catalysts::ApplyResult
complete_exotic_catalyst(std::uint16_t itemDefinitionIndex,
                         std::uint32_t& flags,
                         std::span<std::optional<std::uint16_t>> plugs) noexcept {
    return items::catalysts::apply_completed(itemDefinitionIndex, flags, plugs);
}

bool complete_exotic_catalyst_investment(Family5State& family) noexcept {
    return items::catalysts::append_investment_overrides(family);
}

bool complete_exotic_catalyst_objectives(std::span<std::int32_t> values) noexcept {
    return items::catalysts::append_objective_completions(values);
}

std::uint16_t resolve_exotic_catalyst_effect(std::uint16_t itemDefinitionIndex,
                                             std::uint8_t socketLane,
                                             std::uint16_t plugDefinitionIndex) noexcept {
    return items::catalysts::resolve_effect(itemDefinitionIndex, socketLane, plugDefinitionIndex);
}

bool is_exotic_catalyst_lane(std::uint16_t itemDefinitionIndex, std::uint8_t socketLane) noexcept {
    return items::catalysts::owns_lane(itemDefinitionIndex, socketLane);
}

void set_exotic_catalyst_completion_enabled(bool enabled) noexcept {
    items::catalysts::set_completion_enabled(enabled);
}

} // namespace sunrise::state::build_data
