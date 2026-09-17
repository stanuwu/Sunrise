#include "lobby_chat.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>

#include "../../core/threading/srw_lock.h"
#include "../../middleware/crypto/random_bytes.h"

namespace sunrise::state::social::lobby {
namespace {
template <class T, std::size_t N>
void erase(std::array<T, N>& rows, std::size_t& count, std::size_t index) noexcept {
    for (std::size_t i = index + 1; i < count; ++i) {
        rows[i - 1] = rows[i];
    }
    rows[--count] = {};
}
template <class A> bool member(const A& account, std::uint64_t id) noexcept {
    return std::find(account.memberships.begin(),
                     account.memberships.begin() + account.membershipCount,
                     id)
           != account.memberships.begin() + account.membershipCount;
}
// Protects the bounded mirror; native callback invocation takes place after this lock is released.
core::threading::SrwLock mutex;
Hub hub;
Client client;
bool initialized = false;

bool initialize() noexcept {
    if (initialized) {
        return true;
    }
    std::uint64_t epoch{};
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&epoch, 1)))
        || epoch == 0) {
        return false;
    }
    client.initialize(epoch);
    initialized = true;
    return true;
}
} // namespace

void Hub::sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept {
    if (account >= accounts_.size() || sender == 0 || request.epoch == 0
        || request.membershipCount > kLobbyCapacity || request.messageCount > kBatchCapacity
        || request.receivedThrough >= INT_MAX || request.acceptedThrough == UINT64_MAX) {
        return;
    }
    for (std::size_t i = 0; i < request.membershipCount; ++i) {
        if (!valid_id(request.memberships[i])) {
            return;
        }
    }
    auto& owner = accounts_[account];
    if (owner.epoch == request.epoch && request.receivedThrough >= owner.nextDelivery) {
        return;
    }
    if (owner.epoch != request.epoch) {
        owner = {};
        owner.epoch = request.epoch;
        owner.acceptedThrough = request.acceptedThrough;
        owner.nextDelivery =
            (std::min)(request.receivedThrough, static_cast<std::uint64_t>(INT_MAX)) + 1;
    }
    if (request.membershipRevision >= owner.membershipRevision) {
        owner.memberships = request.memberships;
        owner.membershipCount = request.membershipCount;
        owner.membershipRevision = request.membershipRevision;
    }
    owner.receivedThrough = (std::max)(owner.receivedThrough,
                                       (std::min)(request.receivedThrough, owner.nextDelivery - 1));
    for (std::size_t i = 0; i < owner.pendingCount;) {
        if (owner.pending[i].sequence <= owner.receivedThrough
            || !member(owner, owner.pending[i].lobby)) {
            erase(owner.pending, owner.pendingCount, i);
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < request.messageCount; ++i) {
        const auto& message = request.messages[i];
        if (message.sequence <= owner.acceptedThrough) {
            continue;
        }
        if (message.sequence != owner.acceptedThrough + 1) {
            break;
        }
        if (!member(owner, message.lobby) || message.size > kPayloadCapacity) {
            owner.acceptedThrough = message.sequence;
            continue;
        }
        // Preserve the sender's own echo. Another recipient's stalled callback queue must not
        // block the lobby; its existing deliveries remain intact and this new message is skipped.
        if (owner.pendingCount == kQueueCapacity || owner.nextDelivery > INT_MAX) {
            break;
        }
        for (auto& recipient : accounts_) {
            if (recipient.epoch == 0 || !member(recipient, message.lobby)
                || recipient.pendingCount == kQueueCapacity || recipient.nextDelivery > INT_MAX) {
                continue;
            }
            auto& delivery = recipient.pending[recipient.pendingCount++];
            delivery = message;
            delivery.sender = sender;
            delivery.sequence = recipient.nextDelivery++;
        }
        owner.acceptedThrough = message.sequence;
    }
}

void Hub::feed(std::size_t account, Reply& reply) const noexcept {
    reply = {};
    if (account >= accounts_.size()) {
        return;
    }
    const auto& owner = accounts_[account];
    reply.epoch = owner.epoch;
    reply.membershipRevision = owner.membershipRevision;
    reply.acceptedThrough = owner.acceptedThrough;
    reply.receivedThrough = owner.receivedThrough;
    reply.messageCount = (std::min)(owner.pendingCount, kBatchCapacity);
    for (std::size_t i = 0; i < reply.messageCount; ++i) {
        reply.messages[i] = owner.pending[i];
    }
}
void Hub::disconnect(std::size_t account) noexcept {
    if (account >= accounts_.size()) {
        return;
    }
    auto& owner = accounts_[account];
    owner.memberships = {};
    owner.membershipCount = 0;
    owner.pendingCount = 0;
}
void Hub::reset() noexcept {
    std::destroy_at(this);
    std::construct_at(this);
}
void Hub::forget(std::size_t account) noexcept {
    if (account >= accounts_.size()) {
        return;
    }
    std::destroy_at(&accounts_[account]);
    std::construct_at(&accounts_[account]);
}

void Client::initialize(std::uint64_t epoch) noexcept {
    // Reinitialize in place: the message history is too large for a stack temporary.
    std::destroy_at(this);
    std::construct_at(this);
    request_.epoch = epoch;
    request_.membershipRevision = 1;
}
bool Client::contains(std::uint64_t id) const noexcept {
    return member(request_, id);
}
bool Client::join(std::uint64_t id) noexcept {
    if (!valid_id(id) || request_.epoch == 0) {
        return false;
    }
    if (contains(id)) {
        return true;
    }
    if (request_.membershipCount == kLobbyCapacity) {
        return false;
    }
    request_.memberships[request_.membershipCount++] = id;
    ++request_.membershipRevision;
    return true;
}
void Client::leave(std::uint64_t id) noexcept {
    for (std::size_t i = 0; i < request_.membershipCount; ++i) {
        if (request_.memberships[i] != id) {
            continue;
        }
        erase(request_.memberships, request_.membershipCount, i);
        ++request_.membershipRevision;
        break;
    }
}
bool Client::send(std::uint64_t id, std::span<const std::byte> bytes) noexcept {
    if (!contains(id) || bytes.empty() || bytes.size() > kPayloadCapacity
        || outgoingCount_ == kQueueCapacity || nextSequence_ == UINT64_MAX) {
        return false;
    }
    auto& message = outgoing_[outgoingCount_++];
    message = {};
    message.sequence = nextSequence_++;
    message.lobby = id;
    message.size = static_cast<std::uint16_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), message.body.begin());
    return true;
}
void Client::snapshot(Request& request) const noexcept {
    request = request_;
    request.messageCount = (std::min)(outgoingCount_, kBatchCapacity);
    for (std::size_t i = 0; i < request.messageCount; ++i) {
        request.messages[i] = outgoing_[i];
    }
}
void Client::receive(const Reply& reply) noexcept {
    if (reply.epoch != request_.epoch || reply.messageCount > kBatchCapacity
        || reply.acceptedThrough < request_.acceptedThrough
        || reply.acceptedThrough >= nextSequence_
        || reply.membershipRevision > request_.membershipRevision
        || reply.receivedThrough > request_.receivedThrough) {
        return;
    }
    request_.acceptedThrough = reply.acceptedThrough;
    membershipAck_ = (std::max)(membershipAck_, reply.membershipRevision);
    receiptSent_ = (std::max)(receiptSent_, reply.receivedThrough);
    while (outgoingCount_ && outgoing_[0].sequence <= reply.acceptedThrough) {
        erase(outgoing_, outgoingCount_, 0);
    }
    for (std::size_t i = 0; i < reply.messageCount; ++i) {
        const auto& message = reply.messages[i];
        if (message.sequence <= request_.receivedThrough || message.sequence > INT_MAX
            || message.size > kPayloadCapacity) {
            continue;
        }
        bool seen = false;
        for (std::size_t j = 0; j < incomingCount_; ++j) {
            seen = seen || incoming_[j].sequence == message.sequence;
        }
        if (!seen && incomingCount_ < kQueueCapacity) {
            incoming_[incomingCount_++] = message;
        }
    }
}
bool Client::pending(Message& message) const noexcept {
    if (incomingCount_ == 0) {
        return false;
    }
    message = incoming_[0];
    return true;
}
void Client::delivered(std::uint64_t sequence) noexcept {
    if (!incomingCount_ || incoming_[0].sequence != sequence) {
        return;
    }
    history_[historyNext_] = incoming_[0];
    historyNext_ = (historyNext_ + 1) % history_.size();
    request_.receivedThrough = sequence;
    erase(incoming_, incomingCount_, 0);
}
int Client::read(std::uint64_t id,
                 int index,
                 std::span<std::byte> output,
                 std::uint64_t* sender) const noexcept {
    if (index <= 0) {
        return 0;
    }
    for (const auto& message : history_) {
        if (message.lobby != id || message.sequence != static_cast<std::uint64_t>(index)) {
            continue;
        }
        const auto size = (std::min)(output.size(), static_cast<std::size_t>(message.size));
        std::copy_n(message.body.begin(), size, output.begin());
        if (sender) {
            *sender = message.sender;
        }
        return static_cast<int>(size);
    }
    return 0;
}
bool Client::dirty() const noexcept {
    return request_.epoch != 0
           && (membershipAck_ != request_.membershipRevision || outgoingCount_ != 0
               || receiptSent_ != request_.receivedThrough);
}

bool join(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex);
    return initialize() && client.join(id);
}
void leave(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex);
    client.leave(id);
}
bool send(std::uint64_t id, std::span<const std::byte> bytes) noexcept {
    std::lock_guard lock(mutex);
    return client.send(id, bytes);
}
void snapshot(Request& request) noexcept {
    std::lock_guard lock(mutex);
    client.snapshot(request);
}
void receive(const Reply& reply) noexcept {
    std::lock_guard lock(mutex);
    client.receive(reply);
}
bool pending(Message& message) noexcept {
    std::lock_guard lock(mutex);
    while (client.pending(message)) {
        if (client.contains(message.lobby)) {
            return true;
        }
        client.delivered(message.sequence);
    }
    return false;
}
void delivered(std::uint64_t sequence) noexcept {
    std::lock_guard lock(mutex);
    client.delivered(sequence);
}
int read(std::uint64_t id, int index, std::span<std::byte> output, std::uint64_t* sender) noexcept {
    std::lock_guard lock(mutex);
    return client.read(id, index, output, sender);
}
bool dirty() noexcept {
    std::lock_guard lock(mutex);
    return client.dirty();
}
void sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept {
    std::lock_guard lock(mutex);
    hub.sync(account, sender, request);
}
void feed(std::size_t account, Reply& reply) noexcept {
    std::lock_guard lock(mutex);
    hub.feed(account, reply);
}
void disconnect(std::size_t account) noexcept {
    std::lock_guard lock(mutex);
    hub.disconnect(account);
}
void reset() noexcept {
    std::lock_guard lock(mutex);
    hub.reset();
    client.initialize(0);
    initialized = false;
}

bool contains(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex);
    return client.contains(id);
}
} // namespace sunrise::state::social::lobby
