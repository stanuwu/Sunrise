#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../state/content/content_catalog.h"
#include "../../memory/current_process_memory.h"
#include "../../targets/game.h"
#include "internal.h"

namespace sunrise::client::content::investment {
namespace {

/** FNV-1 hash of the investment-globals bootstrap name, so the name itself is not shipped. */
constexpr std::uint32_t kInvestmentGlobalsNameHash = 0x6F7125CBU;
/** The bootstrap name is not unique, so every match is collected. */
constexpr std::size_t kBootstrapMatchCapacity = 8;

} // namespace

} // namespace sunrise::client::content::investment
