#include "../../runtime.h"
#include "../../runtime/persistence/publication_transaction.h"
#include "insertion_rule_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the plug insertion rules are published. */
bool plug_insertion_rules_ready() noexcept {
    return items::insertion_rules::count() != 0;
}

/** @return True once the installed data has settled that no rule list can be published. */
bool plug_insertion_rules_unsupported() noexcept {
    return items::insertion_rules::unsupported();
}

/** Checks a rule list against the catalog's publication rules. */
bool valid_plug_insertion_rules(std::span<const items::insertion_rules::Rule> rules) noexcept {
    return items::insertion_rules::valid(rules);
}

/** Publishes the plug insertion rules. */
bool publish_plug_insertion_rules(std::span<const items::insertion_rules::Rule> rules) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(items::insertion_rules::replace(rules),
                                 items::insertion_rules::clear);
}

/** Records the settlement, and forgets it when the first cache write after it fails. */
void settle_plug_insertion_rules_unsupported() noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return;
    }
    items::insertion_rules::settle_unsupported();
    static_cast<void>(transaction.finish(true, items::insertion_rules::clear));
}

/** Reads the insertion rules one plug declares. */
bool find_plug_insertion_rule(std::uint16_t plugDefinitionIndex,
                              items::insertion_rules::Rule& rule) noexcept {
    return items::insertion_rules::find(plugDefinitionIndex, rule);
}

} // namespace sunrise::state::build_data
