#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

std::atomic<bool> g_reported{};
std::atomic<bool> g_bucketFailureReported{};
std::atomic<std::size_t> g_abilityFailureReports{};

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

/** Reports the precise ability boundary without flooding the periodic extraction retry. */
void report_ability_failure(const char* stage,
                            std::size_t character,
                            std::size_t first,
                            std::size_t second) noexcept {
    // Extraction retries on every boot pass, so the count is capped to keep one failure readable.
    constexpr std::size_t kReportLimit = 12;
    if (g_abilityFailureReports.fetch_add(1, std::memory_order_relaxed) >= kReportLimit) {
        return;
    }
    std::array<char, 176> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=ability_failure reason=%s character=%zu "
                                      "first=%zu second=%zu",
                                      stage,
                                      character,
                                      first,
                                      second);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports requested, retained, and skipped rows for the equippable-item detail closure. */
void report_detail_count(std::size_t requested, std::size_t built) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=details result=ok requested=%zu rows=%zu "
                                      "skipped=%zu",
                                      requested,
                                      built,
                                      requested - built);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the item index-table walk and every entry it could not read. */
void report_row_count(std::size_t walked,
                      std::size_t rows,
                      std::size_t skipped,
                      bool truncated) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=rows result=ok walked=%zu rows=%zu "
                                      "skipped=%zu truncated=%u",
                                      walked,
                                      rows,
                                      skipped,
                                      static_cast<unsigned>(truncated));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         skipped == 0 && !truncated ? core::log::Level::info
                                                    : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the bounded exact ordinary-socket relation extracted from the installed packages. */
void report_socket_plug_count(std::size_t rules,
                              std::size_t pools,
                              std::size_t members,
                              std::size_t skipped) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=socket_plugs result=ok rules=%zu pools=%zu "
                                      "members=%zu skipped=%zu",
                                      rules,
                                      pools,
                                      members,
                                      skipped);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         skipped == 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the build-scoped catalyst catalog and its first safe failure. */
void report_catalyst_catalog(const state::build_data::items::catalysts::Report& report,
                             bool built) noexcept {
    std::array<char, 192> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=pkg stage=exotic_catalysts result=%s released=%zu placeholder=%zu unsupported=%zu "
        "error=%s item=0x%08X lane=%u",
        built ? "ok" : "fail",
        report.released,
        report.placeholder,
        report.unsupported,
        state::build_data::items::catalysts::error_name(report.error).data(),
        report.itemDefinitionHash,
        static_cast<unsigned>(report.socketLane));
    if (written > 0) {
        const core::log::Level level = !built                    ? core::log::Level::error
                                       : report.unsupported != 0 ? core::log::Level::warn
                                                                 : core::log::Level::info;
        core::log::write(
            core::log::Channel::client, level, {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the installed bucket definition relation used by loadout resolution. */
void report_bucket_equipment_mapping(std::size_t mappedSlots) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=bucket_equipment result=ok rows=%zu mapped=%zu",
                                      tables::kBucketDefinitionCount,
                                      mappedSlots);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one fail-closed bucket-definition rejection without flooding extraction retries. */
void report_bucket_equipment_failure(const char* stage,
                                     std::size_t first,
                                     std::size_t second) noexcept {
    if (g_bucketFailureReported.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=bucket_equipment result=fail reason=%s "
                                      "first=%zu second=%zu",
                                      stage,
                                      first,
                                      second);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Reports the pass outcome once.
 * @param published Rows published, or zero on failure.
 * @param reason Stage the pass reached, named in the line.
 */
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
