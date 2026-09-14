#include "fireteam_membership.h"

#include <algorithm>
#include <limits>

namespace sunrise::state::social::fireteams {
namespace {
bool valid_pair(std::uint64_t first, std::uint64_t second) noexcept {
    return first != 0 && second != 0 && first != second;
}
bool matches(const Edge& edge, std::uint64_t first, std::uint64_t second) noexcept {
    return (edge.joiner == first && edge.target == second)
           || (edge.joiner == second && edge.target == first);
}
constexpr auto maximumRevision = (std::numeric_limits<std::uint64_t>::max)();
} // namespace

bool Membership::request(std::uint64_t joiner, std::uint64_t target, std::uint64_t now) noexcept {
    if (!valid_pair(joiner, target)) {
        return false;
    }
    Edge* vacant{};
    for (auto& edge : edges_) {
        if (edge.established && matches(edge, joiner, target)) {
            return true;
        }
        if (edge.joiner == joiner && edge.target == target) {
            return true;
        }
        if (edge.joiner == 0 && vacant == nullptr) {
            vacant = &edge;
        }
    }
    if (!vacant || revision_ == maximumRevision) {
        return false;
    }
    *vacant = {joiner, target, now, false};
    ++revision_;
    return true;
}

bool Membership::establish(std::uint64_t joiner, std::uint64_t target) noexcept {
    if (!valid_pair(joiner, target)) {
        return false;
    }
    Edge* selected{};
    for (auto& edge : edges_) {
        if (edge.established && matches(edge, joiner, target)) {
            return true;
        }
        if (matches(edge, joiner, target) || (edge.joiner == 0 && selected == nullptr)) {
            selected = &edge;
        }
    }
    if (!selected || revision_ == maximumRevision) {
        return false;
    }
    *selected = {joiner, target, 0, true};
    // A reverse lookup cannot remain pending after the actual relation was admitted.
    for (auto& edge : edges_) {
        if (&edge != selected && matches(edge, joiner, target)) {
            edge = {};
        }
    }
    ++revision_;
    return true;
}

std::size_t
Membership::component(std::uint64_t account,
                      std::array<std::uint64_t, kEdgeCapacity + 1>& members) const noexcept {
    members = {};
    if (account == 0) {
        return 0;
    }
    std::size_t count = 1;
    members[0] = account;
    for (std::size_t cursor = 0; cursor < count; ++cursor) {
        for (const auto& edge : edges_) {
            if (!edge.established) {
                continue;
            }
            const auto peer = edge.joiner == members[cursor]
                                  ? edge.target
                                  : (edge.target == members[cursor] ? edge.joiner : 0);
            if (peer == 0
                || std::find(
                       members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count), peer)
                       != members.begin() + static_cast<std::ptrdiff_t>(count)) {
                continue;
            }
            members[count++] = peer; // A connected component of E edges has at most E+1 vertices.
        }
    }
    return count;
}

bool Membership::connected(std::uint64_t first, std::uint64_t second) const noexcept {
    if (first == 0 || second == 0) {
        return false;
    }
    std::array<std::uint64_t, kEdgeCapacity + 1> members{};
    const auto count = component(first, members);
    return std::find(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count), second)
           != members.begin() + static_cast<std::ptrdiff_t>(count);
}

std::uint64_t Membership::representative(std::uint64_t account) const noexcept {
    std::array<std::uint64_t, kEdgeCapacity + 1> members{};
    const auto count = component(account, members);
    return count <= 1 ? 0
                      : *std::min_element(members.begin(),
                                          members.begin() + static_cast<std::ptrdiff_t>(count));
}

std::uint64_t Membership::pending_target(std::uint64_t joiner) const noexcept {
    std::uint64_t target{};
    if (joiner == 0) {
        return 0;
    }
    for (const auto& edge : edges_) {
        if (edge.joiner != joiner || edge.established) {
            continue;
        }
        if (target != 0 && target != edge.target) {
            return 0;
        }
        target = edge.target;
    }
    return target;
}

bool Membership::depart(std::uint64_t account) noexcept {
    if (account == 0 || revision_ == maximumRevision) {
        return false;
    }
    bool changed{};
    for (auto& edge : edges_) {
        if (edge.joiner == account || edge.target == account) {
            edge = {};
            changed = true;
        }
    }
    if (changed) {
        ++revision_;
    }
    return changed;
}

bool Membership::release(std::uint64_t first, std::uint64_t second) noexcept {
    if (!valid_pair(first, second) || revision_ == maximumRevision) {
        return false;
    }
    bool changed{};
    for (auto& edge : edges_) {
        if (matches(edge, first, second)) {
            edge = {};
            changed = true;
        }
    }
    if (changed) {
        ++revision_;
    }
    return changed;
}

bool Membership::expire(std::uint64_t now) noexcept {
    if (revision_ == maximumRevision) {
        return false;
    }
    bool changed{};
    for (auto& edge : edges_) {
        if (edge.joiner == 0 || edge.established || now < edge.requestedAt
            || now - edge.requestedAt < kJoinTimeoutMs) {
            continue;
        }
        edge = {};
        changed = true;
    }
    if (changed) {
        ++revision_;
    }
    return changed;
}
} // namespace sunrise::state::social::fireteams
