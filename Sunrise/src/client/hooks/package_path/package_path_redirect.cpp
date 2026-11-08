// The client opens each manifest package as `packages\<stem>.pkg` relative to the game folder.
// It builds that path twice: in the package file object, and again in the header cache.
// Mod files live in the Sunrise folder, so their stems get a path relative to the game folder.
// The client keeps the generations it registered at boot, so a live update reaches a tag at load.

#include "package_path_redirect.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/notice/ui_notice_overlay.h"
#include "../../../state/mods/mods_runtime.h"
#include "../../../state/mods/packages/mod_load_patch.h"
#include "../../content/handles/layout.h"
#include "../../hooking/detour.h"
#include "../../memory/current_process_memory.h"
#include "../../targets/game/package_path.h"

namespace sunrise::client::hooks::package_path {
namespace {

/** A manifest stem is at most this long; a longer name is left to the builder. */
constexpr std::size_t kStemInspectionLimit = state::content_manifest::kPackageNameCapacity;
/** Folder the client puts in front of a manifest stem. */
constexpr std::string_view kPackagesPrefix = "packages\\";
/** Extension the client puts after a manifest stem. */
constexpr std::string_view kPackageSuffix = ".pkg";
/** Stack room for one path object copy; the resolved layout must fit. */
constexpr std::size_t kObjectCapacity = 1024;
/** Path objects are copied with 16-byte vector moves. */
constexpr std::size_t kObjectAlignment = 16;

/** Exact ABI of the builder: package file object, then its manifest name. */
using SetPath = char*(__fastcall*)(void* file, const char* name) noexcept;
/** Exact ABI of the opener: path object, access flags, share flags; returns a stdio file. */
using OpenFile = void*(__fastcall*)(const void* object,
                                    std::uint32_t access,
                                    std::uint32_t share) noexcept;
/** Exact ABI of the schema callback dispatcher; the full return register is passed through. */
using SchemaCallback = std::uint64_t(__fastcall*)(void* request,
                                                  std::uint32_t index,
                                                  std::int32_t phase,
                                                  void* slot,
                                                  std::uint32_t tag,
                                                  void* blob,
                                                  void** output) noexcept;

/** Schema callback index of the fixup, the first callback after a tag's bytes land. */
constexpr std::uint32_t kFixupCallback = 2;

/** Detour slots, attached and removed together. */
enum Slot : std::size_t {
    kSetPathSlot,
    kOpenFileSlot,
    kSchemaCallbackSlot,
    kSlotCount,
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<hooking::detour::Handle, kSlotCount> g_handles{};
volatile LONG g_activeCalls{};
/** Copied before the detours attach and never changed while they are attached. */
targets::game::package_path::Targets g_targets{};

/** No member initializers, so nothing runs before ingress counts the call. */
struct CallLease final {
    void* original;
    std::uint32_t pathOffset;
    std::uint32_t pathCapacity;
    std::uint32_t objectRootOffset;
    std::uint32_t objectSize;
};

#if defined(SUNRISE_PACKAGE_PATH_TEST)
volatile LONG g_pauseIngress{};
volatile LONG g_ingressEntered{};
volatile LONG g_releaseIngress{1};
/** A failed fixture cannot leave its worker spinning forever. */
constexpr unsigned kIngressPauseAttempts = 1'000'000'000;
#endif

/** Counts the call before borrowing a trampoline or target layout. */
__declspec(noinline) void ingress(CallLease& lease, Slot slot) noexcept {
#if defined(SUNRISE_PACKAGE_PATH_TEST)
    if (InterlockedCompareExchange(&g_pauseIngress, 0, 1) == 1) {
        (void)InterlockedExchange(&g_ingressEntered, 1);
        for (unsigned attempt = 0; attempt < kIngressPauseAttempts && g_releaseIngress == 0;
             ++attempt) {
            YieldProcessor();
        }
    }
#endif
    (void)InterlockedIncrement(&g_activeCalls);
    AcquireSRWLockShared(&g_lock);
    lease.original = g_handles[slot].original;
    lease.pathOffset = g_targets.pathOffset;
    lease.pathCapacity = g_targets.pathCapacity;
    lease.objectRootOffset = g_targets.objectRootOffset;
    lease.objectSize = g_targets.objectSize;
    ReleaseSRWLockShared(&g_lock);
}

/**
 * WakeByAddressAll makes this non-leaf, so it has the unwind data uninstall needs.
 * Nothing waits on the count.
 */
__declspec(noinline) void egress() noexcept {
    WakeByAddressAll(const_cast<LONG*>(&g_activeCalls));
    (void)InterlockedDecrement(&g_activeCalls);
}

/** Detours calls this with peer threads suspended, so it must not acquire a lock. */
[[nodiscard]] bool idle() noexcept {
    return InterlockedCompareExchange(&g_activeCalls, 0, 0) == 0;
}

/**
 * Finds the mod path for one stem, and logs a miss.
 * @param stage Log stage of the caller.
 * @return True when a loaded mod serves the stem.
 */
[[nodiscard]] bool lookup(std::string_view stem,
                          const char* stage,
                          std::array<char, state::mods::kClientPathCapacity>& path) noexcept {
    if (state::mods::find_client_path(stem, path)) {
        return true;
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=package_path stage=%s stem=%.*s result=miss",
                      stage,
                      static_cast<int>(stem.size()),
                      stem.data());
    return false;
}

/**
 * Writes the mod path when a loaded mod serves this stem, else builds the stock path.
 * @param file Package file object that owns the path field.
 * @param name Manifest stem with no directory and no extension.
 * @return The path field, as the builder returns it.
 */
__declspec(noinline) char*
set_path_body(const CallLease& lease, void* file, const char* name) noexcept {
    const auto original = reinterpret_cast<SetPath>(lease.original);
    if (file == nullptr || name == nullptr) {
        return original(file, name);
    }
    const std::size_t length = strnlen_s(name, kStemInspectionLimit);
    std::array<char, state::mods::kClientPathCapacity> path{};
    if (length == kStemInspectionLimit
        || !lookup(std::string_view(name, length), "redirect", path)) {
        return original(file, name);
    }
    const std::size_t pathLength = strnlen_s(path.data(), path.size());
    if (pathLength >= lease.pathCapacity) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=package_path stage=redirect stem=%s result=fail reason=length",
                          name);
        return original(file, name);
    }
    char* const field = static_cast<char*>(file) + lease.pathOffset;
    std::memcpy(field, path.data(), pathLength + 1);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=package_path stage=redirect stem=%s path=%s result=ok",
                      name,
                      path.data());
    return field;
}

#pragma code_seg(push, package_guard_builder, ".srpkg")
/** The __finally drops the call count even when the original faults. */
__declspec(noinline) char* __fastcall set_path(void* file, const char* name) noexcept {
    CallLease lease;
    ingress(lease, kSetPathSlot);
    char* result{};
    __try {
        result = set_path_body(lease, file, name);
    } __finally {
        egress();
    }
    return result;
}
#pragma code_seg(pop, package_guard_builder)

/** @return The stem of a `packages\<stem>.pkg` path, or an empty view. */
[[nodiscard]] std::string_view manifest_stem(std::string_view path) noexcept {
    if (!path.starts_with(kPackagesPrefix) || !path.ends_with(kPackageSuffix)) {
        return {};
    }
    const std::string_view stem = path.substr(
        kPackagesPrefix.size(), path.size() - kPackagesPrefix.size() - kPackageSuffix.size());
    return stem.find('\\') == std::string_view::npos ? stem : std::string_view{};
}

/**
 * Opens the mod file when a path object names a stem a loaded mod serves, else the stock file.
 * The header cache builds `packages\<stem>.pkg` itself and opens it here.
 * @param object Path object: text, then root id and flags.
 * @return The stdio file, or null when the open failed.
 */
__declspec(noinline) void* open_file_body(const CallLease& lease,
                                          const void* object,
                                          std::uint32_t access,
                                          std::uint32_t share) noexcept {
    const auto original = reinterpret_cast<OpenFile>(lease.original);
    if (object == nullptr) {
        return original(object, access, share);
    }
    const auto* text = static_cast<const char*>(object);
    const std::string_view stem =
        manifest_stem(std::string_view(text, strnlen_s(text, lease.objectRootOffset)));
    std::array<char, state::mods::kClientPathCapacity> path{};
    if (stem.empty() || !lookup(stem, "open", path)) {
        return original(object, access, share);
    }
    const std::size_t pathLength = strnlen_s(path.data(), path.size());
    if (pathLength >= lease.objectRootOffset) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=package_path stage=open stem=%.*s result=fail reason=length",
                          static_cast<int>(stem.size()),
                          stem.data());
        return original(object, access, share);
    }
    // Root and flags stay the caller's; only the text changes.
    alignas(kObjectAlignment) std::array<std::byte, kObjectCapacity> copy{};
    std::memcpy(copy.data(), object, lease.objectSize);
    std::fill_n(copy.begin(), lease.objectRootOffset, std::byte{});
    std::memcpy(copy.data(), path.data(), pathLength);
    void* const file = original(copy.data(), access, share);
    if (file == nullptr) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=package_path stage=open stem=%.*s path=%s result=fail reason=open",
                          static_cast<int>(stem.size()),
                          stem.data(),
                          path.data());
        return file;
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=package_path stage=open stem=%.*s path=%s result=ok",
                      static_cast<int>(stem.size()),
                      stem.data(),
                      path.data());
    return file;
}

#pragma code_seg(push, package_guard_opener, ".srpkg")
/** The __finally drops the call count even when the original faults. */
__declspec(noinline) void* __fastcall open_file(const void* object,
                                                std::uint32_t access,
                                                std::uint32_t share) noexcept {
    CallLease lease;
    ingress(lease, kOpenFileSlot);
    void* result{};
    __try {
        result = open_file_body(lease, object, access, share);
    } __finally {
        egress();
    }
    return result;
}
#pragma code_seg(pop, package_guard_opener)

/**
 * Writes a mod's later generations into a tag the client just loaded from its boot generation.
 * The kept size is the boot generation's entry size, since a kept update never changes it.
 * @param slot The tag's content slot, which the dispatcher passes; it links to the blob.
 * @param kept Bytes the kept updates cover.
 */
void patch_loaded(const void* slot, std::uint32_t tag, void* blob, std::uint32_t kept) noexcept {
    namespace layout = client::content::handles::layout;
    layout::ContentSlot words{};
    const auto at = reinterpret_cast<std::uintptr_t>(slot);
    const bool read = client::memory::copy_current_process(
        nullptr, at, std::as_writable_bytes(std::span(&words, 1)));
    // Word 0 holds the class instead of the size for some entry types; the loading bit may be set.
    const bool sized = (words.sizeWord & layout::kSlotSizeForm) == layout::kSlotSizeForm;
    if (!read || (sized && (words.sizeWord & layout::kSlotSizeMask) < kept)
        || words.classId == layout::kSlotNoClass
        || at - static_cast<std::uintptr_t>(words.link) != reinterpret_cast<std::uintptr_t>(blob)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=package_path stage=load_patch tag=%08X result=fail reason=slot "
                          "size=%08X type=%08X class=%08X",
                          tag,
                          words.sizeWord,
                          words.typeWord,
                          words.classId);
        return;
    }
    (void)state::mods::load_patch::apply(
        tag, words.classId, std::span(static_cast<std::byte*>(blob), kept));
}

/**
 * Patches a tag at its fixup, before any class code reads it, then runs the callback.
 * @param tag The tag hash, which is its runtime handle.
 * @param blob The tag's loaded bytes.
 */
__declspec(noinline) std::uint64_t schema_callback_body(const CallLease& lease,
                                                        void* request,
                                                        std::uint32_t index,
                                                        std::int32_t phase,
                                                        void* slot,
                                                        std::uint32_t tag,
                                                        void* blob,
                                                        void** output) noexcept {
    if (index == kFixupCallback && slot != nullptr && blob != nullptr) {
        if (const std::uint32_t kept = state::mods::load_patch::kept_size(tag); kept != 0) {
            patch_loaded(slot, tag, blob, kept);
        }
    }
    return reinterpret_cast<SchemaCallback>(lease.original)(
        request, index, phase, slot, tag, blob, output);
}

#pragma code_seg(push, package_guard_loader, ".srpkg")
/** The __finally drops the call count even when the original faults. */
__declspec(noinline) std::uint64_t __fastcall schema_callback(void* request,
                                                              std::uint32_t index,
                                                              std::int32_t phase,
                                                              void* slot,
                                                              std::uint32_t tag,
                                                              void* blob,
                                                              void** output) noexcept {
    CallLease lease;
    ingress(lease, kSchemaCallbackSlot);
    std::uint64_t result{};
    __try {
        result = schema_callback_body(lease, request, index, phase, slot, tag, blob, output);
    } __finally {
        egress();
    }
    return result;
}
#pragma code_seg(pop, package_guard_loader)

} // namespace

/** Points mod package stems at their files and patches tags at load. */
bool install() noexcept {
    if (state::mods::file_count() == 0) {
        return true;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_handles[kSetPathSlot].attached) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    bool installed = false;
    if (targets::game::package_path::is_resolved()
        && targets::game::package_path::get().objectSize <= kObjectCapacity) {
        g_targets = targets::game::package_path::get();
        const std::array<hooking::detour::Spec, kSlotCount> specs{
            hooking::detour::Spec{g_targets.setPath, reinterpret_cast<void*>(&set_path)},
            hooking::detour::Spec{g_targets.openFile, reinterpret_cast<void*>(&open_file)},
            hooking::detour::Spec{g_targets.schemaCallback,
                                  reinterpret_cast<void*>(&schema_callback)},
        };
        installed = hooking::detour::install(specs, g_handles);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!installed) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=package_path stage=install result=fail");
        core::ui::notice::raise("Mod packages cannot load: the game's package path was not found.");
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=package_path stage=install result=ok");
    return true;
}

/** @return True when none of the three detours is attached. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&set_path)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&open_file)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&schema_callback)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&ingress)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&egress)},
    };
    // .srpkg holds the three replacement entries and their finally blocks.
    const std::array protectedSpans{hooking::detour::section_span(".srpkg")};
    const bool removed = idle()
                         && (!g_handles[kSetPathSlot].attached
                             || hooking::detour::uninstall(
                                    std::span(g_handles), protectedEntries, protectedSpans, &idle)
                                    == hooking::detour::UninstallResult::removed);
    if (removed) {
        g_targets = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

} // namespace sunrise::client::hooks::package_path
