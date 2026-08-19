#include <algorithm>

#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

class RollbackWriter final : public IPolicyStateWriter {
public:
    RollbackWriter(std::span<std::byte> bytes, std::size_t& size) noexcept
        : bytes_(bytes), size_(&size) {
        *size_ = 0;
    }

    [[nodiscard]] bool write(std::span<const std::byte> bytes) noexcept override {
        if (size_ == nullptr || bytes.size() > bytes_.size() - *size_) {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), bytes_.data() + *size_);
        *size_ += bytes.size();
        return true;
    }

private:
    std::span<std::byte> bytes_{};
    std::size_t* size_{};
};

class RollbackReader final : public IPolicyStateReader {
public:
    explicit RollbackReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool read(std::span<std::byte> bytes) noexcept override {
        if (bytes.size() > bytes_.size() - offset_) {
            return false;
        }
        std::copy_n(bytes_.data() + offset_, bytes.size(), bytes.begin());
        offset_ += bytes.size();
        return true;
    }

    [[nodiscard]] bool consumed_all() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    std::span<const std::byte> bytes_{};
    std::size_t offset_{};
};

} // namespace

/** Saves every observable logical boundary before a writer tick. */
bool WorldRunner::begin_tick_transaction() noexcept {
    if (tickTransactionActive_ || policy_ == nullptr || !commandQueue_.begin_transaction()) {
        return false;
    }
    rollbackPolicyState_ = {};
    RollbackWriter writer(rollbackPolicyState_, rollbackPolicyStateSize_);
    if (!policy_->save(writer)) {
        commandQueue_.rollback_transaction();
        return false;
    }
    rollbackActors_ = actors_;
    rollbackCommandHistory_ = commandHistory_;
    rollbackCommittedEvents_ = committedEvents_;
    rollbackCommittedCommands_ = committedCommands_;
    rollbackMetrics_ = metrics_;
    rollbackTick_ = tick_;
    rollbackPolicySourceSequence_ = policySourceSequence_;
    rollbackCanonicalHash_ = canonicalHash_;
    rollbackCommittedEventCount_ = committedEventCount_;
    rollbackCommittedCommandCount_ = committedCommandCount_;
    rollbackCommandHistoryCount_ = commandHistoryCount_;
    rollbackCommandHistoryCursor_ = commandHistoryCursor_;
    tickTransactionActive_ = true;
    return true;
}

/** Keeps state and ingress produced by one completed writer tick. */
void WorldRunner::commit_tick_transaction() noexcept {
    commandQueue_.commit_transaction();
    tickTransactionActive_ = false;
}

/** Restores logical state, events, commands, policy state, and pending ingress. */
void WorldRunner::rollback_tick_transaction() noexcept {
    if (!tickTransactionActive_) {
        return;
    }
    commandQueue_.rollback_transaction();
    RollbackReader reader(
        std::span<const std::byte>(rollbackPolicyState_).first(rollbackPolicyStateSize_));
    const bool policyRestored =
        policy_ != nullptr && policy_->load(reader) && reader.consumed_all();
    actors_ = rollbackActors_;
    commandHistory_ = rollbackCommandHistory_;
    committedEvents_ = rollbackCommittedEvents_;
    committedCommands_ = rollbackCommittedCommands_;
    metrics_ = rollbackMetrics_;
    tick_ = rollbackTick_;
    policySourceSequence_ = rollbackPolicySourceSequence_;
    canonicalHash_ = rollbackCanonicalHash_;
    committedEventCount_ = rollbackCommittedEventCount_;
    committedCommandCount_ = rollbackCommittedCommandCount_;
    commandHistoryCount_ = rollbackCommandHistoryCount_;
    commandHistoryCursor_ = rollbackCommandHistoryCursor_;
    tickCommands_ = {};
    tickCommandApplied_ = {};
    tickCommandExtension_ = {};
    tickTransactionActive_ = false;
    healthy_ = false;
    static_cast<void>(policyRestored);
}

} // namespace sunrise::server::gameplay::physics::world
