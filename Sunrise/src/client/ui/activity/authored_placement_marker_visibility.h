#pragma once

#include <cstdint>

namespace sunrise::client::ui::activity::authored_placement_marker {

/** SDK page that owns the current world geometry. */
enum class WorldPage : std::uint8_t {
    none,
    objects,
    devices,
    triggers,
    positions,
    squads,
};

namespace visibility_detail {

/** Render ownership retained independently from the page's published geometry identity. */
struct Lease final {
    int visibleFrame{-1};
    WorldPage page{WorldPage::none};
    bool pageActive{};
};

/** @return True when changing pages requires the caller to discard page-owned published rows. */
inline bool set_page(Lease& lease, WorldPage page) noexcept {
    const bool changed = lease.page != page;
    lease.page = page;
    lease.pageActive = page != WorldPage::none;
    if (!lease.pageActive) lease.visibleFrame = -1;
    return changed;
}

/** Grants one frame to a marker producer that is not owned by the World page. */
inline void show_for_frame(Lease& lease, int frame) noexcept {
    lease.visibleFrame = frame;
}

/** Revokes every outstanding World visibility grant without changing the retained page. */
inline void deactivate(Lease& lease) noexcept {
    lease.pageActive = false;
    lease.visibleFrame = -1;
}

/** @return True when a current owner or this exact frame grants marker rendering. */
[[nodiscard]] inline bool visible(const Lease& lease, int frame) noexcept {
    return lease.pageActive || lease.visibleFrame == frame;
}

} // namespace visibility_detail
} // namespace sunrise::client::ui::activity::authored_placement_marker
