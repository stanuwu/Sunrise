#include "queuez_push_reporting.h"

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"

namespace sunrise::server::bap::encrypted::push::queuez_report {
namespace {

/** Widest push report, sized for every field below. */
constexpr std::size_t kReportLimit = 144;
/** Every step report fits one short line. */
constexpr std::size_t kStepLimit = 96;

/**
 * Emits one subscription-step line.
 * @param result Outcome word placed in the result field.
 */
void report_step(const char* result, const char* step) noexcept {
    std::array<char, kStepLimit> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=subscribe result=%s step=%s", result, step);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Reports one appended push with the fields a reference capture can be compared against. */
void push(const char* stage,
          unsigned family,
          std::size_t objects,
          std::size_t bytes,
          int recorded) noexcept {
    std::array<char, kReportLimit> line{};
    const int written =
        recorded < 0 ? std::snprintf(line.data(),
                                     line.size(),
                                     "ev=queuez stage=%s result=ok family=%u objects=%zu bytes=%zu",
                                     stage,
                                     family,
                                     objects,
                                     bytes)
                     : std::snprintf(line.data(),
                                     line.size(),
                                     "ev=queuez stage=%s result=ok family=%u objects=%zu bytes=%zu "
                                     "recorded=%d",
                                     stage,
                                     family,
                                     objects,
                                     bytes,
                                     recorded);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one subscription step that produced no frame. */
void subscription_failure(const char* step) noexcept {
    report_step("fail", step);
}

/** Reports one subscription step that did not record. */
void subscription_state(const char* step) noexcept {
    report_step("unrecorded", step);
}

} // namespace sunrise::server::bap::encrypted::push::queuez_report
