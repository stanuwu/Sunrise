#include <cassert>

#include "../Sunrise/src/client/ui/activity/authored_placement_marker_visibility.h"

using namespace sunrise::client::ui::activity::authored_placement_marker;

int main() {
    visibility_detail::Lease lease{};
    assert(!visibility_detail::visible(lease, 12));

    visibility_detail::show_for_frame(lease, 12);
    assert(visibility_detail::visible(lease, 12));
    visibility_detail::deactivate(lease);
    assert(!visibility_detail::visible(lease, 12));

    assert(visibility_detail::set_page(lease, WorldPage::triggers));
    assert(visibility_detail::visible(lease, 13));
    visibility_detail::show_for_frame(lease, 13);
    visibility_detail::deactivate(lease);
    assert(!visibility_detail::visible(lease, 13));
    assert(lease.page == WorldPage::triggers);

    // Reactivating the same page preserves its published rows; only a page change invalidates them.
    assert(!visibility_detail::set_page(lease, WorldPage::triggers));
    assert(visibility_detail::visible(lease, 14));
    assert(visibility_detail::set_page(lease, WorldPage::objects));
    visibility_detail::show_for_frame(lease, 14);
    assert(visibility_detail::set_page(lease, WorldPage::none));
    assert(!visibility_detail::visible(lease, 14));
}
