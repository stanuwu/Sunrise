#include "activity_host_scriptable_labels.h"

#include <array>
#include <cstddef>

#include "../../../middleware/content/packages/tables/scenario_reader.h"

namespace sunrise::server::ui::activity_host::scriptable_labels {

namespace catalog = state::build_data::scriptables;
namespace tables = middleware::content::packages::tables;

/** @return Stable UI text for one catalog build status. */
const char* status(catalog::BuildStatus value) noexcept {
    switch (value) {
    case catalog::BuildStatus::idle:
        return "idle";
    case catalog::BuildStatus::queued:
        return "queued";
    case catalog::BuildStatus::building:
        return "building";
    case catalog::BuildStatus::ready:
        return "ready";
    case catalog::BuildStatus::failed:
        return "failed";
    }
    return "unknown";
}

/** @return Stable UI text for one package-presence classification. */
const char* presence(catalog::GroupSafety value) noexcept {
    switch (value) {
    case catalog::GroupSafety::notApplicable:
        return "no declared slots";
    case catalog::GroupSafety::destinationSafe:
        return "all destination states";
    case catalog::GroupSafety::bubbleSafe:
        return "all bubble states";
    case catalog::GroupSafety::stateOnly:
        return "selected state only";
    case catalog::GroupSafety::incomplete:
        return "incomplete package read";
    case catalog::GroupSafety::ambiguous:
        return "duplicate identity";
    }
    return "unknown";
}

/** @return Stable UI text for one package-name evidence tier. */
const char* provenance(catalog::NameProvenance value) noexcept {
    switch (value) {
    case catalog::NameProvenance::unresolved:
        return "unresolved";
    case catalog::NameProvenance::packageInline:
        return "package-inline hash match";
    case catalog::NameProvenance::packagePath:
        return "package-path hash match";
    case catalog::NameProvenance::packageIdentifierCandidate:
        return "package identifier candidate";
    }
    return "unresolved";
}

/** @return Stable UI text for one registry descriptor scope. */
const char* scope(std::uint16_t descriptor) noexcept {
    switch (descriptor) {
    case tables::kRegistryFirstDescriptor:
        return "shared/8";
    case tables::kRegistrySecondDescriptor:
        return "registry/24";
    case tables::kRegistryThirdDescriptor:
        return "state-local/40";
    default:
        return "unknown";
    }
}

/** The combo lists the registry descriptors in this order after its "all" entry. */
bool scope_matches(std::uint16_t descriptor, int scope) noexcept {
    // Registry descriptor values the scope combo selects, in its entry order.
    constexpr std::array<std::uint16_t, 4> descriptors{0,
                                                       tables::kRegistryFirstDescriptor,
                                                       tables::kRegistrySecondDescriptor,
                                                       tables::kRegistryThirdDescriptor};
    return scope <= 0 || scope >= static_cast<int>(descriptors.size())
           || descriptor == descriptors[static_cast<std::size_t>(scope)];
}

} // namespace sunrise::server::ui::activity_host::scriptable_labels
