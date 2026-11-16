#include "reward_mail.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace sunrise::state::coop::reward_mail {
namespace {

namespace incident = middleware::bap::activity_message::incident;

/** Raised whenever the event layout changes. */
constexpr std::uint8_t kWireVersion = 2;

#pragma pack(push, 1)
struct EventWire final {
    std::uint8_t version;
    Kind kind;
    std::uint32_t primaryTarget;
    std::array<std::uint32_t, incident::kExtraTargetMaximum> extraTargets;
    std::uint32_t extraTargetCount;
    std::uint32_t payloadLength;
    std::array<std::byte, incident::kPayloadMaximum> payload;
    std::uint8_t hasArea;
    std::uint32_t scenarioTag;
    std::uint32_t bubbleNameHash;
    std::uint8_t packageLength;
    std::array<char, activity::destination::kPackageNameCapacity> package;
};
#pragma pack(pop)
static_assert(sizeof(EventWire) == kEventSize);

/** Every guest's events, oldest first. The host keeps them until each is delivered. */
std::map<std::uint64_t, std::deque<Event>> g_queues;
SRWLOCK g_lock = SRWLOCK_INIT;

} // namespace

/** Packs one event into its fixed wire form. */
bool encode(const Event& event, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < sizeof(EventWire) || event.extraTargetCount > incident::kExtraTargetMaximum
        || event.payloadLength > event.payload.size()
        || event.packageLength > event.package.size()) {
        return false;
    }
    EventWire wire{};
    wire.version = kWireVersion;
    wire.kind = event.kind;
    wire.primaryTarget = event.primaryTarget;
    wire.extraTargets = event.extraTargets;
    wire.extraTargetCount = event.extraTargetCount;
    wire.payloadLength = event.payloadLength;
    wire.payload = event.payload;
    wire.hasArea = event.hasArea ? 1U : 0U;
    wire.scenarioTag = event.scenarioTag;
    wire.bubbleNameHash = event.bubbleNameHash;
    wire.packageLength = event.packageLength;
    wire.package = event.package;
    std::memcpy(output.data(), &wire, sizeof wire);
    written = sizeof wire;
    return true;
}

/** Unpacks one event; a wrong size, version, kind or bound refuses it. */
bool decode(std::span<const std::byte> input, Event& output) noexcept {
    output = {};
    if (input.size() != sizeof(EventWire)) {
        return false;
    }
    EventWire wire{};
    std::memcpy(&wire, input.data(), sizeof wire);
    if (wire.version != kWireVersion || (wire.kind != Kind::interaction && wire.kind != Kind::kill)
        || wire.hasArea > 1 || wire.extraTargetCount > incident::kExtraTargetMaximum
        || wire.payloadLength > wire.payload.size() || wire.packageLength > wire.package.size()) {
        return false;
    }
    output.kind = wire.kind;
    output.primaryTarget = wire.primaryTarget;
    output.extraTargets = wire.extraTargets;
    output.extraTargetCount = wire.extraTargetCount;
    output.payloadLength = wire.payloadLength;
    output.payload = wire.payload;
    output.hasArea = wire.hasArea != 0;
    output.scenarioTag = wire.scenarioTag;
    output.bubbleNameHash = wire.bubbleNameHash;
    output.packageLength = wire.packageLength;
    output.package = wire.package;
    return true;
}

void post(std::uint64_t primarySoid, const Event& event) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    try {
        g_queues[primarySoid].push_back(event);
    } catch (...) {
        // Out of memory loses this one reward; nothing else is touched.
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool front(std::uint64_t primarySoid, Event& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto queue = g_queues.find(primarySoid);
    const bool found = queue != g_queues.end() && !queue->second.empty();
    if (found) {
        output = queue->second.front();
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void pop(std::uint64_t primarySoid) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const auto queue = g_queues.find(primarySoid);
    if (queue != g_queues.end() && !queue->second.empty()) {
        queue->second.pop_front();
        if (queue->second.empty()) {
            g_queues.erase(queue);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::state::coop::reward_mail
