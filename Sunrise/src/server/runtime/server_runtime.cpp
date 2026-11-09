#include "server_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "../../client/network/consumer.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../core/threading/srw_lock.h"
#include "../activity/host_runtime.h"
#include "../activity/mission/mission_script_runtime.h"
#include "../bap/runtime.h"
#include "../gameplay/gameplay_runtime.h"
#include "../http/server_http.h"
#include "../studio/studio_command_link.h"
#include "../transport/bap_listener.h"
#include "../ui/runtime/server_ui_module_runtime.h"

namespace sunrise::server {
namespace {

// The server clock must answer loopback sends independently of the game frame.
constexpr DWORD kServerTickMilliseconds = 10;
core::threading::SrwLock g_lifecycleLock;
HANDLE g_serviceThread{};
HANDLE g_serviceStop{};
bool g_serverInitialized{};
/** Set after a failed start is logged, so a retry each frame does not log again. */
bool g_serviceStartFailed{};

/** The worker never takes the lifecycle lock, so shutdown can join it while holding that lock. */
DWORD WINAPI service_thread(LPVOID) noexcept {
    while (WaitForSingleObject(g_serviceStop, kServerTickMilliseconds) == WAIT_TIMEOUT) {
        service(GetTickCount64());
    }
    return 0;
}

/** The caller holds the lifecycle lock until every worker-owned service slice has ended. */
void stop_service() noexcept {
    if (g_serviceThread != nullptr) {
        SetEvent(g_serviceStop);
        WaitForSingleObject(g_serviceThread, INFINITE);
        CloseHandle(g_serviceThread);
        g_serviceThread = nullptr;
    }
    if (g_serviceStop != nullptr) {
        CloseHandle(g_serviceStop);
        g_serviceStop = nullptr;
    }
}

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
    const std::lock_guard lock(g_lifecycleLock);
    if (g_serverInitialized) {
        return true;
    }
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
            g_serverInitialized = true;
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

/** The runtime owns both handles; a failed start leaves the next activated frame able to retry. */
void start_service() noexcept {
    const std::lock_guard lock(g_lifecycleLock);
    if (!g_serverInitialized || g_serviceThread != nullptr) {
        return;
    }
    g_serviceStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_serviceStop != nullptr) {
        g_serviceThread = CreateThread(nullptr, 0, &service_thread, nullptr, 0, nullptr);
    }
    if (g_serviceThread == nullptr) {
        stop_service();
        if (!g_serviceStartFailed) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::error,
                             "ev=core stage=pump result=server_thread_fail");
        }
        g_serviceStartFailed = true;
        return;
    }
    g_serviceStartFailed = false;
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=core stage=pump result=server_thread");
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
    slice += timed_stage(Stage::mission, "mission", [now] {
        // Cancel disconnected requests before a mission can publish its reload.
        studio::service(now);
        activity::mission::service(now);
    });
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
    const std::lock_guard lock(g_lifecycleLock);
    g_serverInitialized = false;
    g_serviceStartFailed = false;
    stop_service();
    ui::runtime::shutdown();
    stop_gameplay();
    transport::shutdown();
    client::network::unregister_bap_consumer(&bap::consume);
    client::network::unregister_http_consumer(&http::consume);
    bap::shutdown();
    studio::shutdown();
    activity::mission::shutdown();
    activity::host::reset();
    g_window = {};
}

} // namespace sunrise::server
