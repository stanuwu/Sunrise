#include <array>
#include <cassert>
#include "../Sunrise/src/server/bap/activity_host_selection.h"
namespace selection = sunrise::server::bap::host_selection;
int main() {
    // Both directory and public host report 408; actions must use session 2.
    std::array<selection::Candidate, 3> rows{{
        {1, 1, 1, 1, 10, 408, false},
        {2, 3, 1, 1, 11, 408, true},
        {3, 4, 1, 1, 12, 416, true}}};
    assert(selection::current_private(rows) == 0);
    assert(selection::region_host(rows, 0, 408, std::nullopt) == selection::absent);
    assert(selection::region_host(rows, 0, 408, false) == 1);
    assert(selection::region_host(rows, 0, 416, false) == 2);
    assert(selection::region_host(rows, 0, 408, true) == 0);
    assert(selection::region_host(rows, 0, 416, true) == selection::absent);
    rows[1].sourceRevision = 99;
    assert(selection::region_host(rows, 0, 408, false) == selection::absent);
    rows[1].sourceRevision = 1;
    rows[2].region = 408;
    assert(selection::region_host(rows, 0, 408, false) == selection::absent);
    rows[2] = {4, 5, 4, 5, 13, 408, false};
    assert(selection::current_private(rows) == 2);
    assert(selection::region_host(rows, 2, 408, false) == selection::absent);
    assert(selection::region_host(rows, 2, -1, false) == selection::absent);
    assert(selection::current_private({}) == selection::absent);
    assert(selection::region_host(rows, selection::absent, 408, false) == selection::absent);
}
