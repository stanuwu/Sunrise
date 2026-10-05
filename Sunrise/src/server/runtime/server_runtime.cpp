#include "server_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../client/network/consumer.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../activity/host_runtime.h"
#include "../activity/mission/mission_script_runtime.h"
#include "../bap/runtime.h"
#include "../gameplay/gameplay_runtime.h"
#include "../http/server_http.h"
#include "../transport/bap_listener.h"
#include "../ui/runtime/server_ui_module_runtime.h"

namespace sunrise::server {
namespace {
bool gameplayStarted{};
void stop_gameplay() noexcept {
    if (!gameplayStarted) {
        return;
    }
    gameplayStarted = false;
    gameplay::shutdown();
}
} // namespace

/** Registers Server consumers with the Client networking boundary. */
bool initialize() noexcept {
    activity::host::reset();
    activity::mission::initialize();
    if (!client::network::register_http_consumer(&http::consume)) {
        activity::mission::shutdown();
        return false;
    }
    if (client::network::register_bap_consumer(&bap::consume)) {
        // HTTP and UI remain useful when the local BAP port is already owned.
        if (!transport::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=transport stage=listen result=fail");
            if (core::settings::hosts_session() || core::settings::get().server.upstream.enabled) {
                client::network::unregister_bap_consumer(&bap::consume);
                client::network::unregister_http_consumer(&http::consume);
                activity::mission::shutdown();
                return false;
            }
        }
        // The gameplay endpoint must bind before any descriptor advertises it.
        gameplayStarted = !core::settings::get().server.upstream.enabled && gameplay::initialize();
        if (!core::settings::get().server.upstream.enabled && !gameplayStarted) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=gameplay stage=init result=fail");
            if (core::settings::hosts_session()) {
                transport::shutdown();
                client::network::unregister_bap_consumer(&bap::consume);
                client::network::unregister_http_consumer(&http::consume);
                activity::mission::shutdown();
                return false;
            }
        }
        if (ui::runtime::initialize()) {
            return true;
        }
        stop_gameplay();
        transport::shutdown();
        client::network::unregister_bap_consumer(&bap::consume);
    }
    // BAP registration failure rolls back the earlier HTTP registration.
    client::network::unregister_http_consumer(&http::consume);
    activity::mission::shutdown();
    return false;
}

namespace {

// A slice this long blocks the game's loopback sends; the stage that took it is named.
constexpr std::int64_t kSlowStageMilliseconds = 40;
// The server thread ticks every 10 ms; a slice past that delays the next one.
constexpr std::int64_t kTickMicroseconds = 10'000;
// One timing report per window.
constexpr std::uint64_t kWindowMilliseconds = 2'000;

enum class Stage : std::uint8_t { bap, transport, host, mission, gameplay, count };

/** Worst times of one report window. Only the server thread touches it. */
struct Window final {
    std::uint64_t started{};
    std::uint64_t slices{};
    std::uint64_t overTick{};
    std::int64_t worstSlice{};
    std::array<std::int64_t, static_cast<std::size_t>(Stage::count)> worstStage{};
};

Window g_window{};

/** Logs the window's worst times and starts the next window. */
void report_window(std::uint64_t now) noexcept {
    if (g_window.started == 0) {
        g_window.started = now;
        return;
    }
    if (now - g_window.started < kWindowMilliseconds) {
        return;
    }
    const auto& stage = g_window.worstStage;
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=core stage=service result=window slices=%llu over_tick=%llu max_us=%lld "
                      "bap_us=%lld transport_us=%lld host_us=%lld mission_us=%lld gameplay_us=%lld",
                      static_cast<unsigned long long>(g_window.slices),
                      static_cast<unsigned long long>(g_window.overTick),
                      static_cast<long long>(g_window.worstSlice),
                      static_cast<long long>(stage[static_cast<std::size_t>(Stage::bap)]),
                      static_cast<long long>(stage[static_cast<std::size_t>(Stage::transport)]),
                      static_cast<long long>(stage[static_cast<std::size_t>(Stage::host)]),
                      static_cast<long long>(stage[static_cast<std::size_t>(Stage::mission)]),
                      static_cast<long long>(stage[static_cast<std::size_t>(Stage::gameplay)]));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    g_window = {.started = now};
}

/** Runs one stage, records its time, and logs it when it runs past the slow limit. */
template <typename Run> std::int64_t timed_stage(Stage id, const char* name, Run&& stage) noexcept {
    const auto started = std::chrono::steady_clock::now();
    stage();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    auto& worst = g_window.worstStage[static_cast<std::size_t>(id)];
    worst = (std::max)(worst, micros);
    const std::int64_t elapsed = micros / 1'000;
    if (elapsed < kSlowStageMilliseconds) {
        return micros;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=core stage=service result=slow step=%s ms=%lld",
                                      name,
                                      static_cast<long long>(elapsed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return micros;
}

} // namespace

/** Runs one bounded server service slice. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept {
    std::int64_t slice = 0;
    slice += timed_stage(Stage::bap, "bap", [now] { bap::service(now); });
    slice += timed_stage(Stage::transport, "transport", [now] { transport::service(now); });
    slice += timed_stage(Stage::host, "host", [now] { activity::host::service(now); });
    slice += timed_stage(Stage::mission, "mission", [now] { activity::mission::service(now); });
    slice += timed_stage(Stage::gameplay, "gameplay", [now] {
        if (gameplayStarted) {
            gameplay::service(now);
        }
    });
    ++g_window.slices;
    g_window.overTick += slice > kTickMicroseconds ? 1U : 0U;
    g_window.worstSlice = (std::max)(g_window.worstSlice, slice);
    report_window(now);
}

/** Unregisters Server consumers in reverse registration order. */
void shutdown() noexcept {
    ui::runtime::shutdown();
    stop_gameplay();
    transport::shutdown();
    client::network::unregister_bap_consumer(&bap::consume);
    client::network::unregister_http_consumer(&http::consume);
    bap::shutdown();
    activity::mission::shutdown();
    activity::host::reset();
}

} // namespace sunrise::server
