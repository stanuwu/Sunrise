#include "content_readiness_report.h"

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::client::content::diagnostics {
namespace {

/** How many build-data domains the cache-write report covers. */
constexpr std::size_t kDomainCount = 8;

/** One bit per build-data domain, in report order. */
[[nodiscard]] unsigned readiness_mask() noexcept {
    const std::array<bool, kDomainCount> ready{
        state::build_data::named_catalog_ready(),
        state::build_data::item_definitions_ready(),
        state::build_data::configured_item_details_ready(),
        state::build_data::inventory_bucket_descriptors_ready(),
        state::build_data::socket_entry_lists_ready(),
        state::build_data::ability_buckets_ready(),
        state::build_data::progression_definitions_ready(),
        state::build_data::scenario_layouts_ready(),
    };
    unsigned mask = 0;
    for (std::size_t index = 0; index < ready.size(); ++index) {
        mask |= static_cast<unsigned>(ready[index]) << index;
    }
    return mask;
}

/** Sentinel for a mask that has never been reported. */
constexpr unsigned kUnreported = ~0U;
/** One line covers all eight flags. */
constexpr std::size_t kLineCapacity = 192;

std::atomic<unsigned> g_reported{kUnreported};

} // namespace

/** Reports build-data domain readiness, once per change. */
void report_readiness() noexcept {
    const unsigned mask = readiness_mask();
    if (g_reported.exchange(mask, std::memory_order_relaxed) == mask) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=domains named=%u items=%u details=%u "
                                      "buckets=%u sockets=%u abilities=%u progressions=%u "
                                      "scenarios=%u",
                                      mask & 1U,
                                      (mask >> 1U) & 1U,
                                      (mask >> 2U) & 1U,
                                      (mask >> 3U) & 1U,
                                      (mask >> 4U) & 1U,
                                      (mask >> 5U) & 1U,
                                      (mask >> 6U) & 1U,
                                      (mask >> 7U) & 1U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::client::content::diagnostics
