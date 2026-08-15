#pragma once

namespace sunrise::state::build_data::runtime::persistence {

struct Context;

/** Cannot-fail call that drops one target domain after a save failure. */
using Rollback = void() noexcept;

/** Orders one catalog replace against the complete-cache save boundary. */
class Transaction final {
public:
    /** Takes the persistence lock unless a saved cache already freezes all domains. */
    Transaction() noexcept;

    /** Releases a held publish lock when the caller exits before finishing. */
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    /** @return True when a catalog replace is allowed under the held State lock. */
    [[nodiscard]] bool active() const noexcept;

    /**
     * Finishes one replace, and drops its target when the first cache write fails.
     * Dropping clears the target. It never restores what was published before.
     * @param published True when the domain passed validation and replaced its catalog.
     * @param rollback Drops the target domain while the publish lock is held.
     * @return True when the publish works and any complete cache is on disk.
     */
    [[nodiscard]] bool finish(bool published, Rollback& rollback) noexcept;

private:
    Context* state_{};
    bool active_{};
};

} // namespace sunrise::state::build_data::runtime::persistence
