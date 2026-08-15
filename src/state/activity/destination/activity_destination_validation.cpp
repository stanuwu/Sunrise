#include "activity_destination_validation.h"

namespace sunrise::state::activity::destination {

/** Bounds one destination selection against its fixed package-name storage. */
bool valid(const DestinationSelection& selection) noexcept {
    return selection.packageNameLength <= selection.packageName.size();
}

} // namespace sunrise::state::activity::destination
