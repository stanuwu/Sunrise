#include "detour.h"

#include <Windows.h>

#include <array>
#include <detours.h>

#include "detour/transaction/detour_thread_transaction.h"

namespace sunrise::client::hooking::detour {
namespace {

/** 32 hook slots bound each fixed-storage Detours transaction. */
constexpr std::size_t kBatchLimit = 32;

/**
 * Checks every detour description and output slot before transaction setup.
 * @param specs Target and replacement pairs in stable slot order.
 * @param outputs Output handles paired with the descriptions.
 * @return True when the batch can be attached without overwriting state.
 */
[[nodiscard]] bool valid_install_batch(std::span<const Spec> specs,
                                       std::span<const Handle> outputs) noexcept {
    if (specs.empty() || specs.size() != outputs.size() || specs.size() > kBatchLimit) {
        return false;
    }
    for (std::size_t index = 0; index < specs.size(); ++index) {
        if (outputs[index].attached || specs[index].target == nullptr
            || specs[index].replacement == nullptr) {
            return false;
        }
    }
    return true;
}

/**
 * Checks every detour handle before a removal transaction begins.
 * @param handles Handles that must all describe attached detours.
 * @return True when every handle can take part in one fixed transaction.
 */
[[nodiscard]] bool valid_uninstall_batch(std::span<const Handle> handles) noexcept {
    if (handles.empty() || handles.size() > kBatchLimit) {
        return false;
    }
    for (const Handle& handle : handles) {
        if (!handle.attached || handle.original == nullptr || handle.replacement == nullptr) {
            return false;
        }
    }
    return true;
}

/**
 * Queues every detour removal against temporary trampoline pointers.
 * @param handles Attached handles in stable transaction order.
 * @param originals Receives pointer values used by Detours through commit.
 * @return True when every removal was accepted by the active transaction.
 */
[[nodiscard]] bool queue_detach(std::span<const Handle> handles,
                                std::array<void*, kBatchLimit>& originals) noexcept {
    originals = {};
    for (std::size_t index = 0; index < handles.size(); ++index) {
        originals[index] = handles[index].original;
        if (DetourDetach(&originals[index], handles[index].replacement) != NO_ERROR) {
            return false;
        }
    }
    return true;
}

/**
 * Clears handle state only after a removal transaction commits.
 * @param handles Removed handles to clear.
 */
void clear_handles(std::span<Handle> handles) noexcept {
    for (Handle& handle : handles) {
        handle = {};
    }
}

/**
 * Detaches a batch without checking caller-owned protected functions.
 * @param handles Checked attached handles.
 * @return True when every hook was removed and its handle was cleared.
 */
[[nodiscard]] bool uninstall_unprotected(std::span<Handle> handles) noexcept {
    if (!valid_uninstall_batch(handles)) {
        return false;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return false;
    }

    std::array<void*, kBatchLimit> originals{};
    if (!queue_detach(handles, originals)) {
        (void)transaction::abort(threads);
        return false;
    }
    if (!transaction::commit(threads)) {
        return false;
    }

    clear_handles(handles);
    return true;
}

} // namespace

/**
 * Attaches one detour through the batch transaction path.
 * @param spec Target and replacement pair.
 * @param output Receives the committed trampoline.
 * @return True when the detour is attached.
 */
bool install(const Spec& spec, Handle& output) noexcept {
    return install(std::span(&spec, 1), std::span(&output, 1));
}

/**
 * Detaches one detour without caller-owned protected function checks.
 * @param handle Attached detour handle.
 * @return True when the detour is detached.
 */
bool uninstall(Handle& handle) noexcept {
    return uninstall(std::span(&handle, 1));
}

/**
 * Detaches one hook unless a suspended instruction pointer sits inside its replacement.
 * @param handle Attached detour handle.
 * @param replacementActive Receives true when the replacement prevented removal.
 * @return True when the detour is detached.
 */
bool uninstall(Handle& handle, bool& replacementActive) noexcept {
    return uninstall(std::span(&handle, 1), replacementActive);
}

/**
 * Attaches a checked detour batch in one transaction, all or nothing.
 * @param specs Target and replacement pairs in stable slot order.
 * @param outputs Receives trampolines only after a successful commit.
 * @return True when every detour is attached.
 */
bool install(std::span<const Spec> specs, std::span<Handle> outputs) noexcept {
    if (!valid_install_batch(specs, outputs)) {
        return false;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return false;
    }

    std::array<void*, kBatchLimit> originals{};
    for (std::size_t index = 0; index < specs.size(); ++index) {
        originals[index] = specs[index].target;
        if (DetourAttach(&originals[index], specs[index].replacement) != NO_ERROR) {
            // Abort keeps every output detached when one attachment is rejected.
            (void)transaction::abort(threads);
            return false;
        }
    }
    if (!transaction::commit(threads)) {
        return false;
    }

    for (std::size_t index = 0; index < specs.size(); ++index) {
        outputs[index].original = originals[index];
        outputs[index].replacement = specs[index].replacement;
        outputs[index].attached = true;
    }
    return true;
}

/**
 * Detaches a checked batch without caller-owned protected function checks.
 * @param handles Attached handles in their original transaction order.
 * @return True when every detour is removed and every handle is cleared.
 */
bool uninstall(std::span<Handle> handles) noexcept {
    return uninstall_unprotected(handles);
}

/**
 * Detaches a batch unless a suspended instruction pointer sits inside a replacement.
 * @param handles Attached handles in their original transaction order.
 * @param replacementActive Receives true when a replacement prevented removal.
 * @return True when every detour is removed and every handle is cleared.
 */
bool uninstall(std::span<Handle> handles, bool& replacementActive) noexcept {
    replacementActive = false;
    if (handles.empty() || handles.size() > kBatchLimit) {
        return false;
    }

    std::array<ProtectedCodeEntry, kBatchLimit> entries{};
    for (std::size_t index = 0; index < handles.size(); ++index) {
        entries[index].address = handles[index].replacement;
    }
    const UninstallResult result = uninstall(handles, std::span(entries.data(), handles.size()));
    replacementActive = result == UninstallResult::protectedCodeActive;
    return result == UninstallResult::removed;
}

} // namespace sunrise::client::hooking::detour
