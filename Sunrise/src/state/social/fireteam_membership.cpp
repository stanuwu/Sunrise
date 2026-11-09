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

bool Membership::request(std::uint64_t joiner, std::uint64_t target) noexcept {
    if (!valid_pair(joiner, target)) {
        return false;
    }
    Edge* outstanding{};
    Edge* vacant{};
    for (auto& edge : edges_) {
        if (edge.established && matches(edge, joiner, target)) {
            return true;
        }
        // A joiner names one target at a time: a later lookup replaces the earlier intent
        // instead of leaving behind a row no admission will ever consume.
        if (!edge.established && edge.joiner == joiner && outstanding == nullptr) {
            outstanding = &edge;
        }
        if (edge.joiner == 0 && vacant == nullptr) {
            vacant = &edge;
        }
    }
    Edge* const slot = outstanding != nullptr ? outstanding : vacant;
    if (slot == nullptr) {
        return false;
    }
    // Recorded intent moves no revision. A non-established edge is invisible to `connected` and
    // `representative`, so counting it would let a lookup refuse a shared allocation whose plan
    // depends only on admitted relations.
    *slot = {joiner, target, false};
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
    *selected = {joiner, target, true};
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
} // namespace sunrise::state::social::fireteams
