/**
 * Test input for an automated in-game run. The game reads keys by polling and the mouse as raw
 * input, which posted messages cannot reach, and only while its window is in front. A test process
 * reports the window in front, a held key through the polled-input guard, a test cursor, and a
 * marked raw-input handle as one mouse button record.
 */

#include "test_input.h"

#include <Windows.h>

#include <Xinput.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../hooks/polled_input/runtime.h"
#include "../module_range.h"

namespace sunrise::client::diagnostics::test_input {
namespace {

/** Process environment switch; a saved user or machine variable is never needed. */
constexpr wchar_t kSwitch[] = L"SUNRISE_TEST_INPUT";
/** Loading from System32 stops the guard taking an export from the game directory. */
constexpr wchar_t kUserModule[] = L"user32.dll";
/** RID_INPUT and RID_HEADER, the two GetRawInputData commands. */
constexpr UINT kRawInput = RID_INPUT, kRawHeader = RID_HEADER;
/** GetRawInputData reports a failure with all bits set. */
constexpr UINT kRawFailure = static_cast<UINT>(-1);

using GetForegroundWindowFunction = HWND(WINAPI*)();
using GetCursorPosFunction = BOOL(WINAPI*)(LPPOINT);
using GetRawInputDataFunction = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

/** Export slots, in transaction order. */
enum Slot : std::size_t { foreground, cursor, rawInput, slotCount };
// The user32 exports the diagnostic replaces.
constexpr std::array<const char*, slotCount> kExports{
    "GetForegroundWindow", "GetCursorPos", "GetRawInputData"};

std::array<hooking::detour::Handle, slotCount> g_handles{};
std::array<void*, slotCount> g_targets{};
HMODULE g_module{};
ModuleRange g_gameRange{};
ModuleRange g_ownRange{};
std::atomic<HWND> g_window{};
/** Test cursor in client pixels, packed x low and y high; negative means none. */
std::atomic<std::int64_t> g_cursor{-1};
std::atomic_bool g_installed{};
std::atomic_bool g_shotPending{};
/** XInput library the game loads its pad state from. */
constexpr wchar_t kPadModule[] = L"xinput9_1_0.dll";
using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
/** Pad-state exports the test pad replaces: two plain exports and the extended one. */
constexpr std::size_t kPadSlots = 3;
std::array<hooking::detour::Handle, kPadSlots> g_padHandles{};
std::array<void*, kPadSlots> g_padTargets{};
std::array<HMODULE, 2> g_padModules{};
std::array<std::atomic<std::uint32_t>, kPadSlots> g_padReads{};
/** Test pad 0: buttons low 16 bits, left stick x and y as signed 16-bit halves above them. */
std::atomic<std::uint64_t> g_pad{};
/** Test pad 0's right stick: x and y as signed 16-bit halves. */
std::atomic<std::uint32_t> g_padRight{};
std::atomic<DWORD> g_padPacket{};
/** Process environment path a shot is written to. */
constexpr wchar_t kShotPath[] = L"SUNRISE_TEST_SHOT";

/** @return One 8-bit channel from a unit float, clamped. */
[[nodiscard]] std::uint8_t unit_byte(float value) noexcept {
    const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    return static_cast<std::uint8_t>(clamped * 255.0F + 0.5F);
}

/** @return A float from IEEE half bits. */
[[nodiscard]] float half_float(std::uint16_t bits) noexcept {
    const std::uint32_t sign = (bits & 0x8000U) << 16U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;
    std::uint32_t value = sign;
    if (exponent == 0x1FU) {
        value |= 0x7F800000U | (mantissa << 13U);
    } else if (exponent != 0 || mantissa != 0) {
        value |= ((exponent == 0 ? 1U : exponent) + 112U) << 23U | (mantissa << 13U);
    }
    float result{};
    std::memcpy(&result, &value, sizeof result);
    return result;
}

/**
 * Converts one row texel to blue, green, red, alpha bytes.
 * @return False for a format this shot does not read.
 */
[[nodiscard]] bool
texel(DXGI_FORMAT format, const std::uint8_t* source, std::uint8_t* out) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        std::memcpy(out, source, 4);
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        out[0] = source[2], out[1] = source[1], out[2] = source[0], out[3] = source[3];
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t word{};
        std::memcpy(&word, source, sizeof word);
        out[2] = static_cast<std::uint8_t>((word & 0x3FFU) >> 2U);
        out[1] = static_cast<std::uint8_t>(((word >> 10U) & 0x3FFU) >> 2U);
        out[0] = static_cast<std::uint8_t>(((word >> 20U) & 0x3FFU) >> 2U);
        out[3] = 255;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<std::uint16_t, 4> lanes{};
        std::memcpy(lanes.data(), source, sizeof lanes);
        out[2] = unit_byte(half_float(lanes[0])), out[1] = unit_byte(half_float(lanes[1]));
        out[0] = unit_byte(half_float(lanes[2])), out[3] = 255;
        return true;
    }
    default:
        return false;
    }
}

/**
 * Writes a mapped staging texture as a top-down 32-bit BMP.
 * @return "ok", or the failed step.
 */
[[nodiscard]] const char* write_bitmap(ID3D11DeviceContext* context,
                                       ID3D11Texture2D* staging,
                                       const D3D11_TEXTURE2D_DESC& desc,
                                       const wchar_t* path) noexcept {
    const std::uint32_t texelBytes = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U;
    std::array<std::uint8_t, 4> probe{};
    const std::array<std::uint8_t, 8> zero{};
    if (!texel(desc.Format, zero.data(), probe.data())) {
        return "format";
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return "map";
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        context->Unmap(staging, 0);
        return "file";
    }
    const std::uint32_t rowBytes = desc.Width * 4U;
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER info{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof fileHeader + sizeof info;
    fileHeader.bfSize = fileHeader.bfOffBits + rowBytes * desc.Height;
    info.biSize = sizeof info;
    info.biWidth = static_cast<LONG>(desc.Width);
    // A negative height stores rows top first, as the texture holds them.
    info.biHeight = -static_cast<LONG>(desc.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    DWORD written = 0;
    bool ok = WriteFile(file, &fileHeader, sizeof fileHeader, &written, nullptr) != FALSE
              && WriteFile(file, &info, sizeof info, &written, nullptr) != FALSE;
    std::vector<std::uint8_t> row(rowBytes);
    for (std::uint32_t y = 0; ok && y < desc.Height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData)
                             + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (std::uint32_t x = 0; x < desc.Width; ++x) {
            (void)texel(desc.Format,
                        source + static_cast<std::size_t>(x) * texelBytes,
                        row.data() + x * 4U);
        }
        ok = WriteFile(file, row.data(), rowBytes, &written, nullptr) != FALSE;
    }
    context->Unmap(staging, 0);
    CloseHandle(file);
    return ok ? "ok" : "write";
}

template <typename Function> [[nodiscard]] Function original(Slot slot) noexcept {
    void* entry = g_handles[slot].original != nullptr ? g_handles[slot].original : g_targets[slot];
    return reinterpret_cast<Function>(entry);
}

/** @return True when the call came from the game image. */
[[nodiscard]] bool from_game(const void* caller) noexcept {
    return contains(g_gameRange, reinterpret_cast<std::uintptr_t>(caller));
}

/** Reports the game's window in front to game code once a test message named it. */
__declspec(noinline) HWND WINAPI get_foreground_window() noexcept {
    const HWND window = g_window.load(std::memory_order_relaxed);
    if (window != nullptr && from_game(_ReturnAddress())) {
        return window;
    }
    const auto next = original<GetForegroundWindowFunction>(foreground);
    return next != nullptr ? next() : nullptr;
}

/**
 * Reports the test cursor to every caller but this DLL, or a point just outside the window when
 * none is set. The user's real cursor must never steer a background run.
 */
__declspec(noinline) BOOL WINAPI get_cursor_pos(LPPOINT point) noexcept {
    const std::int64_t packed = g_cursor.load(std::memory_order_relaxed);
    const HWND window = g_window.load(std::memory_order_relaxed);
    if (window != nullptr && point != nullptr
        && !contains(g_ownRange, reinterpret_cast<std::uintptr_t>(_ReturnAddress()))) {
        point->x = packed >= 0 ? static_cast<LONG>(packed & 0xFFFF) : -1;
        point->y = packed >= 0 ? static_cast<LONG>((packed >> 16) & 0xFFFF) : -1;
        return ClientToScreen(window, point);
    }
    const auto next = original<GetCursorPosFunction>(cursor);
    return next != nullptr ? next(point) : FALSE;
}

/** Answers a marked handle with one mouse record whose button flags the handle carries. */
__declspec(noinline) UINT WINAPI get_raw_input_data(
    HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(input);
    if ((value & kRawMarkMask) != kRawMark || size == nullptr
        || headerSize != sizeof(RAWINPUTHEADER)) {
        const auto next = original<GetRawInputDataFunction>(rawInput);
        return next != nullptr ? next(input, command, data, size, headerSize) : kRawFailure;
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=test_input stage=raw command=0x%X buttons=0x%X",
                      command,
                      static_cast<unsigned>(value & 0xFFFF));
    RAWINPUT record{};
    record.header.dwType = RIM_TYPEMOUSE;
    record.header.dwSize = sizeof(RAWINPUT);
    record.header.wParam = RIM_INPUT;
    record.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
    record.data.mouse.usButtonFlags = static_cast<USHORT>(value & 0xFFFF);
    // The game moves its menu cursor by relative counts, carried as two signed 16-bit halves.
    record.data.mouse.lLastX = static_cast<std::int16_t>((value >> 32U) & 0xFFFF);
    record.data.mouse.lLastY = static_cast<std::int16_t>((value >> 48U) & 0xFFFF);
    const UINT needed = command == kRawHeader ? sizeof(RAWINPUTHEADER) : sizeof(RAWINPUT);
    if (command != kRawInput && command != kRawHeader) {
        return kRawFailure;
    }
    if (data == nullptr) {
        *size = needed;
        return 0;
    }
    if (*size < needed) {
        return kRawFailure;
    }
    std::memcpy(data, &record, needed);
    return needed;
}

/** Fills pad 0 from the test state; false when the call is not for pad 0. */
[[nodiscard]] bool test_pad(DWORD user, XINPUT_STATE* state, std::size_t slot) noexcept {
    if (g_padReads[slot].fetch_add(1, std::memory_order_relaxed) == 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=test_input stage=pad_read slot=%zu user=%lu",
                          slot,
                          static_cast<unsigned long>(user));
    }
    if (user != 0 || state == nullptr) {
        return false;
    }
    const std::uint64_t pad = g_pad.load(std::memory_order_relaxed);
    *state = XINPUT_STATE{};
    state->dwPacketNumber = g_padPacket.load(std::memory_order_relaxed);
    state->Gamepad.wButtons = static_cast<WORD>(pad & 0xFFFF);
    state->Gamepad.sThumbLX = static_cast<SHORT>((pad >> 16U) & 0xFFFF);
    state->Gamepad.sThumbLY = static_cast<SHORT>((pad >> 32U) & 0xFFFF);
    const std::uint32_t right = g_padRight.load(std::memory_order_relaxed);
    state->Gamepad.sThumbRX = static_cast<SHORT>(right & 0xFFFF);
    state->Gamepad.sThumbRY = static_cast<SHORT>(right >> 16U);
    return true;
}

/** Calls the pad export a slot replaced. */
[[nodiscard]] DWORD pad_next(std::size_t slot, DWORD user, XINPUT_STATE* state) noexcept {
    void* entry =
        g_padHandles[slot].original != nullptr ? g_padHandles[slot].original : g_padTargets[slot];
    return entry != nullptr ? reinterpret_cast<XInputGetStateFunction>(entry)(user, state)
                            : static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);
}

template <std::size_t Slot>
__declspec(noinline) DWORD WINAPI xinput_get_state(DWORD user, XINPUT_STATE* state) noexcept {
    return test_pad(user, state, Slot) ? static_cast<DWORD>(ERROR_SUCCESS)
                                       : pad_next(Slot, user, state);
}

/**
 * Attaches the test pad on every distinct pad-state export the game may call: the plain export of
 * both XInput libraries and the extended one, ordinal 100. A game without them still gets the rest.
 */
void install_pad() noexcept {
    const std::array<HMODULE, 2> modules{
        LoadLibraryExW(kPadModule, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32),
        LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    g_padModules = modules;
    const std::array<void*, kPadSlots> found{
        modules[0] != nullptr
            ? reinterpret_cast<void*>(GetProcAddress(modules[0], "XInputGetState"))
            : nullptr,
        modules[1] != nullptr
            ? reinterpret_cast<void*>(GetProcAddress(modules[1], "XInputGetState"))
            : nullptr,
        modules[1] != nullptr
            ? reinterpret_cast<void*>(GetProcAddress(modules[1], MAKEINTRESOURCEA(100)))
            : nullptr};
    const std::array<void*, kPadSlots> replacements{reinterpret_cast<void*>(&xinput_get_state<0>),
                                                    reinterpret_cast<void*>(&xinput_get_state<1>),
                                                    reinterpret_cast<void*>(&xinput_get_state<2>)};
    for (std::size_t slot = 0; slot < kPadSlots; ++slot) {
        bool repeat = false;
        for (std::size_t earlier = 0; earlier < slot; ++earlier) {
            repeat = repeat || found[earlier] == found[slot];
        }
        if (found[slot] == nullptr || repeat) {
            continue;
        }
        if (hooking::detour::install({found[slot], replacements[slot]}, g_padHandles[slot])) {
            g_padTargets[slot] = found[slot];
        } else {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=test_input stage=pad_install slot=%zu result=fail",
                              slot);
        }
    }
}

} // namespace

bool enabled() noexcept {
    static const bool value = [] {
        std::array<wchar_t, 8> text{};
        const DWORD length =
            GetEnvironmentVariableW(kSwitch, text.data(), static_cast<DWORD>(text.size()));
        return length == 1 && text[0] == L'1';
    }();
    return value;
}

/** Attaches the input, cursor and pad guards in a test process; does nothing otherwise. */
bool install() noexcept {
    if (!enabled() || g_installed.load(std::memory_order_acquire)) {
        return g_installed.load(std::memory_order_acquire);
    }
    HMODULE own = nullptr;
    if (!module_range(GetModuleHandleW(nullptr), g_gameRange)
        || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&install),
                              &own)
               == 0
        || !module_range(own, g_ownRange)) {
        return false;
    }
    g_module = LoadLibraryExW(kUserModule, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool resolved = g_module != nullptr;
    for (std::size_t slot = 0; resolved && slot < slotCount; ++slot) {
        g_targets[slot] = reinterpret_cast<void*>(GetProcAddress(g_module, kExports[slot]));
        resolved = g_targets[slot] != nullptr;
    }
    const std::array<hooking::detour::Spec, slotCount> specs{
        hooking::detour::Spec{g_targets[foreground],
                              reinterpret_cast<void*>(&get_foreground_window)},
        hooking::detour::Spec{g_targets[cursor], reinterpret_cast<void*>(&get_cursor_pos)},
        hooking::detour::Spec{g_targets[rawInput], reinterpret_cast<void*>(&get_raw_input_data)},
    };
    if (!resolved || !hooking::detour::install(specs, g_handles)) {
        g_targets = {};
        if (g_module != nullptr) {
            (void)FreeLibrary(g_module);
            g_module = nullptr;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=test_input stage=install result=fail");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    // The game polls for pads once at startup, so the test pad must be connected from the start.
    install_pad();
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=test_input stage=install result=ok");
    return true;
}

/** Detaches every guard install attached. */
void uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire) || !hooking::detour::uninstall(g_handles)) {
        return;
    }
    g_installed.store(false, std::memory_order_release);
    g_targets = {};
    (void)FreeLibrary(g_module);
    g_module = nullptr;
    bool padsDetached = true;
    for (std::size_t slot = 0; slot < kPadSlots; ++slot) {
        if (g_padTargets[slot] != nullptr && hooking::detour::uninstall(g_padHandles[slot])) {
            g_padTargets[slot] = nullptr;
        }
        padsDetached = padsDetached && g_padTargets[slot] == nullptr;
    }
    // A module stays loaded while a pad detour still points into it.
    if (padsDetached) {
        for (HMODULE& module : g_padModules) {
            if (module != nullptr) {
                (void)FreeLibrary(module);
                module = nullptr;
            }
        }
    }
}

bool foreign_mouse(UINT message, WPARAM& word) noexcept {
    if (message < WM_MOUSEFIRST || message > WM_MOUSELAST || !enabled()) {
        return false;
    }
    if ((word & kMouseMarkMask) != kMouseMark) {
        return true;
    }
    word &= ~kMouseMarkMask;
    return false;
}

bool foreign_raw_input(LPARAM value) noexcept {
    return enabled() && (static_cast<std::uintptr_t>(value) & kRawMarkMask) != kRawMark;
}

/** Applies one posted test message; real mouse leaves are dropped. */
bool handle(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    if ((message != kKeyMessage && message != kCursorMessage && message != kShotMessage
         && message != kPadMessage && message != WM_MOUSELEAVE)
        || !enabled()) {
        return false;
    }
    // The real cursor is not the test cursor, so its leave would cancel a posted click.
    if (message == WM_MOUSELEAVE) {
        return true;
    }
    g_window.store(window, std::memory_order_relaxed);
    if (message == kPadMessage) {
        g_pad.store(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
        g_padRight.store(static_cast<std::uint32_t>(word), std::memory_order_relaxed);
        g_padPacket.fetch_add(1, std::memory_order_relaxed);
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=test_input stage=pad buttons=0x%04X reads=%u,%u,%u",
                          static_cast<unsigned>(value & 0xFFFF),
                          g_padReads[0].load(std::memory_order_relaxed),
                          g_padReads[1].load(std::memory_order_relaxed),
                          g_padReads[2].load(std::memory_order_relaxed));
        return true;
    }
    if (message == kShotMessage) {
        g_shotPending.store(true, std::memory_order_release);
        return true;
    }
    if (message == kCursorMessage) {
        g_cursor.store(value < 0 ? -1 : static_cast<std::int64_t>(value & 0xFFFFFFFF),
                       std::memory_order_relaxed);
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=test_input stage=cursor x=%d y=%d",
                          static_cast<int>(value & 0xFFFF),
                          static_cast<int>((value >> 16) & 0xFFFF));
        return true;
    }
    const auto key = static_cast<std::uint32_t>(word);
    if (value != 0) {
        hooks::polled_input::hold_key(key);
    } else {
        hooks::polled_input::release_key();
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=test_input stage=key key=0x%02X held=%d",
                      key,
                      value != 0 ? 1 : 0);
    return true;
}

/**
 * Copies the back buffer to a CPU-readable texture and writes it as a bitmap.
 * @param desc Receives the back buffer's description.
 * @return The result word for the log line.
 */
[[nodiscard]] const char* save_frame(IDXGISwapChain* swapChain,
                                     ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     const wchar_t* path,
                                     D3D11_TEXTURE2D_DESC& desc) noexcept {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(
            swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        return "no_buffer";
    }
    back->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC copy = desc;
    copy.Usage = D3D11_USAGE_STAGING;
    copy.BindFlags = 0;
    copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    copy.MiscFlags = 0;
    copy.SampleDesc = {1, 0};
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    const char* result = "no_staging";
    if (desc.SampleDesc.Count == 1
        && SUCCEEDED(device->CreateTexture2D(&copy, nullptr, &staging))) {
        context->CopyResource(staging, back);
        result = write_bitmap(context, staging, desc, path);
        staging->Release();
    }
    back->Release();
    return result;
}

/** Saves the frame about to be presented when a test asked for one. */
void capture(IDXGISwapChain* swapChain,
             ID3D11Device* device,
             ID3D11DeviceContext* context) noexcept {
    if (!g_shotPending.exchange(false, std::memory_order_acq_rel) || swapChain == nullptr
        || device == nullptr || context == nullptr) {
        return;
    }
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length =
        GetEnvironmentVariableW(kShotPath, path.data(), static_cast<DWORD>(path.size()));
    D3D11_TEXTURE2D_DESC desc{};
    const char* const result = length == 0 || length >= path.size()
                                   ? "no_path"
                                   : save_frame(swapChain, device, context, path.data(), desc);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=test_input stage=shot result=%s width=%u height=%u format=%u",
                      result,
                      desc.Width,
                      desc.Height,
                      static_cast<unsigned>(desc.Format));
}

} // namespace sunrise::client::diagnostics::test_input
