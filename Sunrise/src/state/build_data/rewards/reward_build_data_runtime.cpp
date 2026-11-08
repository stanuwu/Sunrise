#include "../runtime.h"
#include "../runtime/persistence/build_data_persistence.h"
#include "../runtime/persistence/publication_transaction.h"
#include "reward_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the reward pools and item wrappers are published. */
bool reward_definitions_ready() noexcept {
    return rewards::ready();
}

/** @return True when the installed data refused the reward graph for this build. */
bool reward_definitions_unsupported() noexcept {
    return runtime::persistence::context().rewardsUnsupported.load();
}

/** Records the refusal beside the other build-bound facts; a clear forgets it. */
void settle_reward_definitions_unsupported() noexcept {
    runtime::persistence::context().rewardsUnsupported.store(true);
}

/** Publishes the complete reward graph in one step. */
bool publish_reward_definitions(rewards::View definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(rewards::replace(definitions), rewards::clear);
}

/** Checks a reward graph against the catalog's publication rules. */
bool valid_reward_definitions(rewards::View definitions) noexcept {
    return rewards::valid(definitions);
}

/** Reads one item's reward row. */
bool find_reward_item(std::uint16_t itemIndex, rewards::Item& item) noexcept {
    return rewards::find_item(itemIndex, item);
}

/** Lends the complete reward graph to one callback under the catalog's read lock. */
bool read_reward_definitions(void* context,
                             bool (*consume)(void*, rewards::View) noexcept) noexcept {
    return rewards::read(context, consume);
}

} // namespace sunrise::state::build_data
