#include "command_queue.h"

#include <utility>

namespace sunrise::izanami::editor::commands {

/** Adds one editor command for later game-thread processing. */
void CommandQueue::push(Command command) {
    const std::lock_guard lock{mutex_};
    queue_.push_back(std::move(command));
}

/** @return True when one command was removed from the queue. */
bool CommandQueue::try_pop(Command& command) {
    const std::lock_guard lock{mutex_};
    if (queue_.empty()) {
        return false;
    }
    command = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

/** Drops every pending command. */
void CommandQueue::clear() {
    const std::lock_guard lock{mutex_};
    queue_.clear();
}

/** @return Current pending command count. */
std::size_t CommandQueue::size() const {
    const std::lock_guard lock{mutex_};
    return queue_.size();
}

/** Stores a committed transaction and clears redo history. */
void TransactionHistory::commit(Transaction transaction) {
    undo_.push_back(std::move(transaction));
    redo_.clear();
}

/** @return True when an undo transaction exists. */
bool TransactionHistory::can_undo() const {
    return !undo_.empty();
}

/** @return True when a redo transaction exists. */
bool TransactionHistory::can_redo() const {
    return !redo_.empty();
}

/** @return Number of transactions available for undo. */
std::size_t TransactionHistory::undo_count() const {
    return undo_.size();
}

/** @return Number of transactions available for redo. */
std::size_t TransactionHistory::redo_count() const {
    return redo_.size();
}

/** Moves the latest committed transaction to redo history and returns its undo side. */
bool TransactionHistory::undo(Transaction& transaction) {
    if (undo_.empty()) {
        return false;
    }
    transaction = undo_.back();
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

/** Moves the latest redo transaction back to undo history and returns its redo side. */
bool TransactionHistory::redo(Transaction& transaction) {
    if (redo_.empty()) {
        return false;
    }
    transaction = redo_.back();
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    return true;
}

/** Clears every stored transaction. */
void TransactionHistory::clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace sunrise::izanami::editor::commands
