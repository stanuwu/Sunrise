/**
 * Arms one derived-state rebuild after the family-five object commit. The account's unlock
 * overrides reach that object only when the commit finishes, so a rebuild armed any earlier
 * reads an override list that is not there yet.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::network::investment {
namespace {

/**
 * The family-five object commit. Its prologue alone matches a dozen sites, some of which differ
 * only in their call displacements, so the exact trailing run is what makes this unique. Every
 * relative displacement is a wildcard.
 */
constexpr std::string_view kCommitSignatureText =
    "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 E8 ? ? ? ? 48 8B C8 48 8B D3 E8 ? ? ? ? 48 8D "
    "44 24 ? C6 47 50 02 A8 03 75 ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kCommitSignature =
    signature<signature_length(kCommitSignatureText)>(kCommitSignatureText);

/** Result returned when the trampoline is gone, so no commit ran. */
constexpr std::int64_t kNoCommit = 0;

using CommitFamily5 = std::int64_t(__fastcall*)(void*, std::uint64_t*);

hooking::detour::Handle g_handle{};
std::atomic<CommitFamily5> g_original{nullptr};
std::atomic_bool g_reportedArm{false};

/**
 * Runs the family-five commit, then arms one derived-state rebuild. The two callers pass different
 * second arguments, so it is passed on unread. Arming twice is harmless, and the next freshness
 * verdict uses it up, so repeat commits need no latch.
 * @param primaryRecordBlock Borrowed record block the commit writes into.
 * @param nested4 Borrowed caller-owned argument, passed on unread.
 * @return The commit's own result, or the no-commit result when the trampoline is gone.
 */
__declspec(noinline) std::int64_t __fastcall commit(void* primaryRecordBlock,
                                                    std::uint64_t* nested4) noexcept {
    const CommitFamily5 original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return kNoCommit;
    }
    // Arm on the way out: the overrides are in the object only once the commit has run.
    const std::int64_t result = original(primaryRecordBlock, nested4);
    arm_derived_rebuild();
    if (!g_reportedArm.exchange(true, std::memory_order_relaxed)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=investment stage=family5_commit result=armed");
    }
    return result;
}

} // namespace

/**
 * Attaches the family-five commit rearm.
 * @return True when the target is found and the detour attaches.
 */
bool install_family5_rearm() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kCommitSignature, "queuez_family5_commit");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=family5_commit result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&commit)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=investment stage=family5_commit result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<CommitFamily5>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=investment stage=family5_commit result=ok");
    return true;
}

/** @return True when the family-five commit rearm is absent. */
bool uninstall_family5_rearm() noexcept {
    if (g_handle.attached && !hooking::detour::uninstall(g_handle)) {
        return false;
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reportedArm.store(false, std::memory_order_release);
    return true;
}

/** @return True while the family-five commit rearm is attached. */
bool family5_rearm_is_installed() noexcept {
    return g_handle.attached;
}

} // namespace sunrise::client::hooks::network::investment
