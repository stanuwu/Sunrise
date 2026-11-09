#include "studio_cui_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "../../../middleware/studio/studio_mapping.h"
#include "studio_cui_protocol.h"
#include "studio_cui_reader.h"
#include "studio_protocol.h"

namespace sunrise::client::diagnostics::studio {

#if defined(SUNRISE_CUI_LINK_TEST)
/** Test seam: runs between the two loads of one request id. */
void cui_link_test_between_loads(volatile std::int32_t& request) noexcept;
#endif

namespace {

namespace wire = cui_wire;
/** FNV-1a 64 offset basis and prime; the hash only detects a changed section. */
constexpr std::uint64_t kFnvBasis = 0xCBF29CE484222325ULL, kFnvPrime = 0x100000001B3ULL;
/** captureMicros is in microseconds. */
constexpr std::int64_t kMicrosPerSecond = 1'000'000;
/** The editor's node field carries the node id in its low 16 bits. */
constexpr std::uint32_t kNodeMask = 0xFFFF;

SRWLOCK g_cuiLock{SRWLOCK_INIT};
bool g_accepting{};
HANDLE g_mapping{};
wire::SharedCui* g_view{};
std::uint64_t g_nextConnect{}, g_nextCapture{};
/** What this connection last published; a disconnect forgets it so a new editor gets it all. */
bool g_screensPublished{}, g_treePublished{};
std::uint64_t g_screensHash{}, g_treeHash{};
std::uint32_t g_treeResponse{}, g_treeRevision{}, g_detailAnswered{};
wire::Status g_treeStatus{};
std::array<wire::ScreenRow, wire::kScreenCapacity> g_screenRows{};
std::array<wire::WidgetRow, wire::kWidgetCapacity> g_treeRows{};

/** One editor request, copied out of the mapping. */
struct Target final {
    std::uint32_t id{}, hierarchy{}, ordinal{}, node{};
};

template <typename T>
[[nodiscard]] std::uint64_t hash_bytes(std::uint64_t hash, std::span<const T> values) noexcept {
    for (const std::byte value : std::as_bytes(values)) {
        hash = (hash ^ static_cast<std::uint64_t>(value)) * kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint32_t load(volatile std::int32_t& word) noexcept {
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&word), 0, 0));
}

void bump(volatile std::int32_t& sequence) noexcept {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&sequence));
}

/**
 * Copies one editor request; fill copies its target fields into target.
 * @return False when there is no request, or a newer id means the target may be torn.
 */
template <typename Fill>
[[nodiscard]] bool
read_request(volatile std::int32_t& request, Target& target, const Fill& fill) noexcept {
    target.id = load(request);
    fill(target);
#if defined(SUNRISE_CUI_LINK_TEST)
    cui_link_test_between_loads(request);
#endif
    return target.id != 0 && load(request) == target.id;
}

/** Forgets what was published, so the next editor connection gets every section again. */
void disconnect() noexcept {
    middleware::studio::close_editor_view(g_mapping, g_view);
    g_screensPublished = false;
    g_treePublished = false;
    g_detailAnswered = 0;
}

/** @return True only when an editor created the matching CUI protocol. */
[[nodiscard]] bool connect() noexcept {
    g_view = middleware::studio::open_editor_view<wire::SharedCui>(wire::kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    // Studio reads clientVersion to explain a version mismatch.
    if (g_view->control.magic == wire::kMagic) {
        g_view->control.clientVersion = wire::kVersion;
    }
    if (!middleware::studio::editor_header_ok(
            g_view->control, wire::kMagic, wire::kVersion, wire::kMappingBytes)) {
        disconnect();
        return false;
    }
    return true;
}

/** Republishes the open screens when they changed. */
void serve_screens(std::uint64_t now) noexcept {
    CuiScreens screens{};
    if (cui_manager_ready()) {
        (void)read_cui_screens(g_screenRows, screens);
    }
    const auto rows = std::span(g_screenRows).first(screens.count);
    const std::uint64_t hash =
        hash_bytes(hash_bytes(kFnvBasis, std::span<const CuiScreens>(&screens, 1)),
                   std::span<const wire::ScreenRow>(rows));
    if (g_screensPublished && hash == g_screensHash) {
        return;
    }
    auto& output = g_view->screens;
    bump(output.sequence);
    output.count = screens.count;
    output.time = now;
    output.flags = screens.flags;
    output.focus0 = screens.focus0;
    output.focus1 = screens.focus1;
    std::copy(rows.begin(), rows.end(), output.rows.begin());
    bump(output.sequence);
    g_screensPublished = true;
    g_screensHash = hash;
}

/** Answers the current watch; the tree is republished only for a new id or a changed tree. */
void serve_watch(std::uint64_t now) noexcept {
    auto& control = g_view->control;
    Target target{};
    if (!read_request(control.watchRequest, target, [&](Target& output) {
            output.hierarchy = control.watchHierarchy;
            output.ordinal = control.watchOrdinal;
        })) {
        return;
    }
    CuiTree tree{};
    std::uintptr_t screen{};
    const CuiLookup lookup = cui_manager_ready()
                                 ? find_cui_screen(target.hierarchy, target.ordinal, screen)
                                 : CuiLookup::faulted;
    auto status = lookup == CuiLookup::absent ? wire::Status::absent : wire::Status::unavailable;
    if (lookup == CuiLookup::found) {
        const bool walked = walk_cui_tree(screen, g_treeRows, tree);
        status = walked ? wire::Status::complete : wire::Status::unavailable;
        tree.count = walked ? tree.count : 0;
    }
    const auto rows = std::span(g_treeRows).first(tree.count);
    const std::uint64_t hash = hash_bytes(hash_bytes(kFnvBasis, std::span<const CuiTree>(&tree, 1)),
                                          std::span<const wire::WidgetRow>(rows));
    if (g_treePublished && target.id == g_treeResponse && status == g_treeStatus
        && hash == g_treeHash) {
        return;
    }
    auto& output = g_view->tree;
    bump(output.sequence);
    output.response = target.id;
    output.time = now;
    output.status = status;
    output.count = tree.count;
    output.flags = tree.flags;
    output.revision = ++g_treeRevision;
    output.hierarchyTag = tree.hierarchyTag;
    output.screenTag = tree.screenTag;
    output.viewHash = tree.viewHash;
    output.total = tree.total;
    std::copy(rows.begin(), rows.end(), output.rows.begin());
    bump(output.sequence);
    g_treePublished = true;
    g_treeResponse = target.id;
    g_treeStatus = status;
    g_treeHash = hash;
}

/** @return The answer status; answer holds the record only when complete. */
[[nodiscard]] wire::Status describe_widget(const Target& target,
                                           wire::DetailSection& answer) noexcept {
    std::uintptr_t screen{}, node{};
    CuiLookup lookup = cui_manager_ready()
                           ? find_cui_screen(target.hierarchy, target.ordinal, screen)
                           : CuiLookup::faulted;
    if (lookup == CuiLookup::found) {
        lookup = find_cui_node(
            screen, target.hierarchy, static_cast<std::uint16_t>(target.node & kNodeMask), node);
    }
    if (lookup == CuiLookup::found && read_cui_detail(node, answer)) {
        return wire::Status::complete;
    }
    answer = {};
    return lookup == CuiLookup::absent ? wire::Status::absent : wire::Status::unavailable;
}

/** Each widget request id is answered once. */
void serve_widget(std::uint64_t now) noexcept {
    auto& control = g_view->control;
    Target target{};
    if (!read_request(control.widgetRequest,
                      target,
                      [&](Target& output) {
                          output.hierarchy = control.widgetHierarchy;
                          output.ordinal = control.widgetOrdinal;
                          output.node = control.widgetNode;
                      })
        || target.id == g_detailAnswered) {
        return;
    }
    wire::DetailSection answer{};
    answer.status = describe_widget(target, answer);
    answer.response = target.id;
    answer.time = now;
    auto& output = g_view->detail;
    bump(output.sequence);
    std::memcpy(reinterpret_cast<std::byte*>(&output) + wire::kResponseOffset,
                reinterpret_cast<const std::byte*>(&answer) + wire::kResponseOffset,
                wire::kDetailBytes - wire::kResponseOffset);
    bump(output.sequence);
    g_detailAnswered = target.id;
}

/**
 * One capture: connect, check the editor, then serve screens, the watch and one widget request.
 * The caller holds the CUI lock.
 * @return True when Studio is attached.
 */
[[nodiscard]] bool capture(std::uint64_t now) noexcept {
    if (g_view == nullptr) {
        if (now < g_nextConnect) {
            return false;
        }
        g_nextConnect = now + kConnectInterval;
        if (!connect()) {
            return false;
        }
    }
    auto& control = g_view->control;
    control.gamePid = GetCurrentProcessId();
    control.bridgeFlags = cui_manager_ready() ? wire::managerReady : 0U;
    if (!middleware::studio::editor_alive(control.heartbeat, now, kHeartbeatTimeout)) {
        disconnect();
        return false;
    }
    LARGE_INTEGER begin{}, end{}, frequency{};
    QueryPerformanceCounter(&begin);
    InterlockedExchange64(&control.checkedTime, static_cast<LONG64>(now));
    serve_screens(now);
    serve_watch(now);
    serve_widget(now);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    control.captureMicros = static_cast<std::uint32_t>((end.QuadPart - begin.QuadPart)
                                                       * kMicrosPerSecond / frequency.QuadPart);
    return true;
}

} // namespace

/** Waits for a capture in progress like close does, so the two never interleave. */
void start_cui_mapping() noexcept {
    AcquireSRWLockExclusive(&g_cuiLock);
    g_accepting = true;
    ReleaseSRWLockExclusive(&g_cuiLock);
}

/** A busy lock means another frame is capturing, so this frame skips. */
bool observe_cui_frame(std::uint64_t now) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_cuiLock)) {
        return false;
    }
    bool captured = false;
    if (g_accepting && now >= g_nextCapture) {
        g_nextCapture = now + wire::kCaptureInterval;
        captured = capture(now);
    }
    ReleaseSRWLockExclusive(&g_cuiLock);
    return captured;
}

/** After this returns no capture runs until start_cui_mapping. */
void close_cui_mapping() noexcept {
    AcquireSRWLockExclusive(&g_cuiLock);
    g_accepting = false;
    disconnect();
    g_nextConnect = 0;
    g_nextCapture = 0;
    ReleaseSRWLockExclusive(&g_cuiLock);
}

} // namespace sunrise::client::diagnostics::studio
