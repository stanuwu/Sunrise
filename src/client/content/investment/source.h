#pragma once

#include <cstdint>

#include "../handles/handle_resolver.h"

namespace sunrise::client::content::investment {

/** Runtime inputs shared by the installed investment-table extractors. */
struct Source {
    /** Installed runtime tag for the investment-globals blob. */
    std::uint32_t investmentGlobalsTag{};
    /** Loaded-content handle tables and bounded memory reader. */
    handles::Source handles{};
};

} // namespace sunrise::client::content::investment
