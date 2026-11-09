#include "../../runtime.h"
#include "../../runtime/persistence/publication_transaction.h"
#include "insertion_rule_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the plug insertion rules are published. */
bool plug_insertion_rules_ready() noexcept {
    return items::insertion_rules::count() != 0;
}

/** Publishes the plug insertion rules. */
bool publish_plug_insertion_rules(std::span<const items::insertion_rules::Rule> rules) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(items::insertion_rules::replace(rules),
                                 items::insertion_rules::clear);
}

/** Reads the insertion rules one plug declares. */
bool find_plug_insertion_rule(std::uint16_t plugDefinitionIndex,
                              items::insertion_rules::Rule& rule) noexcept {
    return items::insertion_rules::find(plugDefinitionIndex, rule);
}

} // namespace sunrise::state::build_data
