#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/pursuits/definition.h"

namespace sunrise::client::content::items::packages {

class RewardConditions;

/** Logs a refusal the installed data decides and settles the objective and pursuit domains. */
void settle_pursuits_unsupported(const char* reason) noexcept;

/** Collects pursuit contracts during the item walk that fills the cache. */
class PursuitBuild {
public:
    /** Starts a new walk, discarding what an earlier one collected. */
    void begin() noexcept;
    /**
     * Collects one item's contract, returning false when it could not be stored.
     * @param questStep The item's place in its quest set.
     * @param valueMap Blob containing all four unlock value maps, which map its completion values.
     * @param conditions Loaded condition tables its reward rows bind through.
     */
    [[nodiscard]] bool item(const middleware::content::packages::tables::items::Row& row,
                            const state::build_data::pursuits::QuestStep& questStep,
                            std::span<const std::byte> valueMap,
                            const RewardConditions& conditions) noexcept;
    /** Counts an item the installed data leaves unreadable. That item gets no contract. */
    void unread(std::uint16_t index) noexcept;
    /**
     * Notes a cataloged item whose read or storage failed for a reason the installed data did not
     * decide. Publication waits for a walk with no such failure.
     */
    void read_failed(std::uint16_t index) noexcept;
    /**
     * Publishes the collected contracts beside the item catalog, dropping any whose item the
     * catalog lacks. Contracts that still fail validation settle the domain as unsupported.
     */
    void publish() noexcept;

private:
    std::vector<state::build_data::pursuits::Definition> rows_;
    std::size_t unreadCount_{};
    std::uint16_t firstUnread_{state::build_data::pursuits::kAbsent};
    std::size_t readFailures_{};
    std::uint16_t firstReadFailure_{state::build_data::pursuits::kAbsent};
    std::size_t unreadableConditions_{};
};

} // namespace sunrise::client::content::items::packages
