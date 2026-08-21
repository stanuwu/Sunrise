#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../definition.h"
#include "../details/definition.h"
#include "../item_catalog.h"
#include "../socket_plugs/definition.h"
#include "definition.h"

namespace sunrise::state::build_data::items::catalysts {

/** Build-scoped facts that package structure cannot identify safely. */
struct Facts {
    std::uint32_t imageTimestamp{};
    std::uint32_t imageSize{};
    /** Historical release state is not present in the installed Season 11 item relations. */
    std::span<const std::uint32_t> releasedWeaponHashes;
};

/** Installed domains used to derive exact item, socket, and completed-plug relations. */
struct Source {
    BuildIdentity build;
    std::span<const items::Definition> items;
    std::span<const details::Definition> details;
    std::span<const socket_plugs::Rule> socketPlugRules;
    std::span<const socket_plugs::Pool> socketPlugPools;
    std::span<const socket_plugs::Member> socketPlugMembers;
    /** Dense item-indexed completion conditions read from item definition expressions. */
    std::span<const CompletionCondition> completionConditions;
    /** Dense socket-type-indexed acquired-state gates read from socket type definitions. */
    std::span<const AcquisitionGate> acquisitionGates;
    /** Dense objective-indexed completion values read from the installed objective table. */
    std::span<const std::int32_t> objectiveCompletionValues;
};

/** @return The generated facts pinned to Destiny 2 build 86657.20.08.23. */
[[nodiscard]] Facts generated_facts() noexcept;

/**
 * @param build Installed executable identity.
 * @param facts Build-scoped catalyst facts.
 * @return True when the facts apply to the installed executable.
 */
[[nodiscard]] bool supports_build(const BuildIdentity& build, const Facts& facts) noexcept;

/**
 * Derives all released and placeholder catalyst records without display text.
 * On failure, count is zero and no partial record is visible.
 * @param source Installed build domains and executable identity.
 * @param facts Build-scoped role and release facts.
 * @param output Fixed storage that receives the complete catalog.
 * @param count Receives the number of used output rows.
 * @param report Receives counts and the first unsafe released relation.
 * @return True when all released relations are clear and the catalog fits.
 */
[[nodiscard]] bool derive(const Source& source,
                          const Facts& facts,
                          std::span<Definition> output,
                          std::size_t& count,
                          Report& report) noexcept;

/**
 * Re-derives a catalog and compares every row with a stored candidate.
 * @param source Installed build domains and executable identity.
 * @param facts Build-scoped role and release facts.
 * @param definitions Candidate catalog to verify.
 * @return True when the candidate exactly matches one safe derivation.
 */
[[nodiscard]] bool matches_derived(const Source& source,
                                   const Facts& facts,
                                   std::span<const Definition> definitions) noexcept;

/**
 * Rebuilds package-only transient relations from a stored catalog, then re-derives it.
 * The stored acquisition and completion flag rows must still name installed items.
 * Conflicting rows fail closed.
 * @param source Installed cache domains. Its transient spans are ignored.
 * @param facts Build-scoped role and release facts.
 * @param definitions Stored candidate catalog.
 * @return True when one fresh derivation exactly matches the stored catalog.
 */
[[nodiscard]] bool matches_cached(const Source& source,
                                  const Facts& facts,
                                  std::span<const Definition> definitions) noexcept;

} // namespace sunrise::state::build_data::items::catalysts
