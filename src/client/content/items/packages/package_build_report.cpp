#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

std::atomic<bool> g_reported{};

} // namespace

/** @param slot Requested-set position. @param definitionIndex Native item index that failed. */
void report_detail_failure(std::size_t slot, std::uint16_t definitionIndex) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=details result=fail slot=%zu index=%u",
                                      slot,
                                      static_cast<unsigned>(definitionIndex));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @param count Ability bucket rows the pass built, one per subclass and ability selection. */
void report_ability_count(std::size_t count) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=pkg stage=abilities result=ok rows=%zu", count);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @param count Detail rows the pass built, covering equipped items and every plug they socket. */
void report_detail_count(std::size_t count) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=pkg stage=details result=ok rows=%zu", count);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the pass outcome once. @param published Rows published, or zero on failure. */
void report(std::size_t published, const char* reason) noexcept {
    if (g_reported.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::array<char, 96> line{};
    const int written = published != 0
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=build_data stage=items result=ok rows=%zu",
                                            published)
                            : std::snprintf(line.data(),
                                            line.size(),
                                            "ev=build_data stage=items result=fail reason=%s",
                                            reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         published != 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::client::content::items::packages
