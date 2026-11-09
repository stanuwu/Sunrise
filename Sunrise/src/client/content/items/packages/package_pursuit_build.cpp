#include "package_pursuit_build.h"

#include <algorithm>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace pursuits = state::build_data::pursuits;

} // namespace

void settle_pursuits_unsupported(const char* reason) noexcept {
    if (state::build_data::pursuit_catalog_unsupported()) {
        return;
    }
    state::build_data::settle_pursuit_catalog_unsupported();
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=pursuits result=unsupported reason=%s",
                      reason);
}

/** @return True when the pursuit contracts are published or can never be. */
bool pursuit_catalog_settled() noexcept {
    return state::build_data::pursuit_definitions_ready()
           || state::build_data::pursuit_catalog_unsupported();
}

void PursuitBuild::begin() noexcept {
    rows_.clear();
    unreadCount_ = 0;
    firstUnread_ = pursuits::kAbsent;
    readFailures_ = 0;
    firstReadFailure_ = pursuits::kAbsent;
}

bool PursuitBuild::item(const tables::items::Row& row) noexcept {
    // Only pursuits declare completion metadata. An unreadable metadata block still gets a
    // contract, marked unreadable.
    if (!row.hasCompletionMetadata && row.objectiveMetadataValid) {
        return true;
    }
    try {
        pursuits::Definition output{};
        output.definitionIndex = row.definitionIndex;
        output.definitionHash = row.definitionHash;
        output.readable = row.objectiveMetadataValid && row.hasCompletionMetadata;
        output.lifetimeSeconds = row.lifetimeSeconds;
        if (output.readable) {
            output.requireAllObjectives = row.requireAllObjectives;
            output.requireNonOptionalCompletion = row.requireNonOptionalCompletion;
            std::copy_n(row.objectiveIndices, row.objectiveCount, output.objectives.begin());
            output.objectiveCount = row.objectiveCount;
        }
        rows_.push_back(output);
        return true;
    } catch (...) {
        return false;
    }
}

void PursuitBuild::unread(std::uint16_t index) noexcept {
    if (unreadCount_++ == 0) {
        firstUnread_ = index;
    }
}

void PursuitBuild::read_failed(std::uint16_t index) noexcept {
    if (readFailures_++ == 0) {
        firstReadFailure_ = index;
    }
}

void PursuitBuild::publish() noexcept {
    // Every contract is checked against the item and objective tables, so wait for both.
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::objective_definitions_ready()) {
        return;
    }
    // A failed read or allocation left out contracts the data declares. Publishing now would cache
    // the catalog without them, and validating it could settle the domain unsupported over a
    // failure the data did not decide.
    if (readFailures_ != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=pursuits result=fail reason=item_read read_failures=%zu "
                          "first_index=%u",
                          readFailures_,
                          static_cast<unsigned>(firstReadFailure_));
        return;
    }
    // A row an earlier pass failed to read stays out of the published item catalog, so its
    // contract is pruned with it rather than settling the domain over an item State never holds.
    const std::size_t pruned = std::erase_if(rows_, [](const pursuits::Definition& row) noexcept {
        state::build_data::items::Definition item{};
        return !state::build_data::find_item_definition_index(row.definitionIndex, item);
    });
    const bool valid = state::build_data::valid_pursuit_definitions(rows_);
    // The cache write can still fail, and a failed publish repeats the item walk on a later pass.
    const bool published = valid && state::build_data::publish_pursuit_definitions(rows_);
    const std::size_t rowCount = rows_.size();
    // State keeps its own copy, so release the collected rows.
    rows_ = {};
    if (!valid) {
        settle_pursuits_unsupported("contracts");
        return;
    }
    if (!published) {
        return;
    }
    core::log::writef(core::log::Channel::client,
                      unreadCount_ == 0 && pruned == 0 ? core::log::Level::info
                                                       : core::log::Level::warn,
                      "ev=pkg stage=pursuits result=ready rows=%zu unread=%zu first_unread=%u "
                      "pruned=%zu",
                      rowCount,
                      unreadCount_,
                      static_cast<unsigned>(firstUnread_),
                      pruned);
}

} // namespace sunrise::client::content::items::packages
