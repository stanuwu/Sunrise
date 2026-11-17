#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../state/activity/definition.h"

namespace sunrise::server::activity::mission::event_launcher {

/** Bounds the sites one freeroam program can advertise to the overlay. */
inline constexpr std::size_t kMaxSites = 64;
/** The longest manual countdown is the normal five-minute rally lead. */
inline constexpr int kMaxRallySeconds = 300;
/** Site and area display names, including the terminator. */
using Label = std::array<char, 64>;

/** One playable site declared by its mission, indexed within that region's rotation. */
struct Site {
    Label location{};
    Label name{};
    int region{};
    int ordinal{};
};

/** Site zero ends the event in the region and returns it to its normal rotation. */
struct Request {
    std::int64_t serial{};
    int region{-1};
    int site{};
    int rallySeconds{};
};

/** A render-thread copy of the destination menu and its local public host. */
struct Menu {
    std::array<Site, kMaxSites> sites{};
    std::size_t count{};
    state::activity::SessionBinding owner{};
    Request request{};
    Label phase{};
    int currentSite{};
    std::int64_t rallySeconds{};
    std::int64_t acknowledged{};
    bool manual{};
    bool scriptFailed{};
    /** The public host's script stopped on a callback error; no request reaches it. */
    bool scriptStopped{};
};

/** Reads the local destination's sites; only its arrived public session can accept a request. */
void read(const state::activity::SessionBinding& local, int region, Menu& menu) noexcept;

/** Queues one start or cancellation against the same session generation shown in the menu. */
[[nodiscard]] bool submit(const state::activity::SessionBinding& owner,
                          int region,
                          int site,
                          int delaySeconds) noexcept;

} // namespace sunrise::server::activity::mission::event_launcher
