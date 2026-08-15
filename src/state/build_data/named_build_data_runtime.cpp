#include "../content/content_catalog.h"
#include "runtime.h"
#include "runtime/domain_markers.h"
#include "runtime/persistence/publication_transaction.h"

namespace sunrise::state::build_data {

namespace {

/** Drops the ready flag, then removes the named candidate so extraction can run again. */
void rollback_named_publication() noexcept {
    runtime::named::clear();
    content::clear();
}

} // namespace

/** @return True when named package mappings are complete in State. */
bool named_catalog_ready() noexcept {
    return runtime::named::ready();
}

/** Marks the filled named catalog complete, and saves once every domain is ready. */
bool publish_named_catalog() noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !content::seal()) {
        return false;
    }
    runtime::named::publish();
    return transaction.finish(true, rollback_named_publication);
}

} // namespace sunrise::state::build_data
