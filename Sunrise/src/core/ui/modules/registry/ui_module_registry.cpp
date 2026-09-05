#include "ui_module_registry.h"

#include <Windows.h>

namespace sunrise::core::ui::modules::registry {
namespace {

/** One registry position, holding a copied descriptor when occupied. */
struct Slot {
    Descriptor descriptor;
    bool occupied{};
};

/** Three domains own every page. */
constexpr std::size_t kOwnerCount = 3;
/** Menu order of the single module list. */
constexpr std::array<Owner, kOwnerCount> kMenuOrder{Owner::client, Owner::server, Owner::core};

std::array<Slot, kModuleCapacity> g_slots{};
std::uint64_t g_revision{};
SRWLOCK g_registryLock{SRWLOCK_INIT};

/** @return True for a defined owner. */
[[nodiscard]] bool is_valid_owner(Owner owner) noexcept {
    return owner == Owner::client || owner == Owner::server || owner == Owner::core;
}

/** @return True when the slot holds this exact ID. */
[[nodiscard]] bool has_id(const Slot& slot, std::string_view stableId) noexcept {
    return slot.descriptor.stable_id() == stableId;
}

} // namespace

/** @return Every descriptor, in registration-slot order. */
std::span<const Descriptor> RegistrySnapshot::entries() const noexcept {
    return {entries_.data(), count_};
}

/** @return Registry revision captured with the descriptor range. */
std::uint64_t RegistrySnapshot::revision() const noexcept {
    return revision_;
}

/** Copies one immutable descriptor into the fixed registry. */
RegistrationResult register_module(const Descriptor& descriptor) noexcept {
    if (!is_valid(descriptor)) {
        return RegistrationResult::invalidDescriptor;
    }

    AcquireSRWLockExclusive(&g_registryLock);
    Slot* available = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.occupied && has_id(slot, descriptor.stable_id())) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return RegistrationResult::duplicateStableId;
        }
        if (!slot.occupied && available == nullptr) {
            // The scan goes on after a free slot is found, so duplicates are still caught.
            available = &slot;
        }
    }
    if (available == nullptr) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return RegistrationResult::capacityReached;
    }
    available->descriptor = descriptor;
    available->occupied = true;
    ++g_revision;
    ReleaseSRWLockExclusive(&g_registryLock);
    return RegistrationResult::registered;
}

/** Removes one module by its globally unique stable ID. */
bool unregister_module(std::string_view stableId) noexcept {
    if (stableId.empty()) {
        return false;
    }
    AcquireSRWLockExclusive(&g_registryLock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && has_id(slot, stableId)) {
            slot = {};
            ++g_revision;
            ReleaseSRWLockExclusive(&g_registryLock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_registryLock);
    return false;
}

/** @return A copy of every module, taken under the lock, in menu order. */
RegistrySnapshot snapshot() noexcept {
    RegistrySnapshot result;
    AcquireSRWLockShared(&g_registryLock);
    result.revision_ = g_revision;
    // One pass per owner groups the list without sorting it, so registration order is kept
    // inside each group.
    for (const Owner owner : kMenuOrder) {
        for (const Slot& slot : g_slots) {
            if (slot.occupied && slot.descriptor.owner() == owner) {
                result.entries_[result.count_++] = slot.descriptor;
            }
        }
    }
    ReleaseSRWLockShared(&g_registryLock);
    return result;
}

/** @param owner Domain whose registrations are all removed under one lock. */
void clear(Owner owner) noexcept {
    if (!is_valid_owner(owner)) {
        return;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&g_registryLock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.descriptor.owner() == owner) {
            slot = {};
            changed = true;
        }
    }
    if (changed) {
        // One owner clear counts as one revision change.
        ++g_revision;
    }
    ReleaseSRWLockExclusive(&g_registryLock);
}

/** Clears every registration. The revision keeps rising. */
void clear() noexcept {
    bool changed = false;
    AcquireSRWLockExclusive(&g_registryLock);
    for (Slot& slot : g_slots) {
        if (slot.occupied) {
            slot = {};
            changed = true;
        }
    }
    if (changed) {
        ++g_revision;
    }
    ReleaseSRWLockExclusive(&g_registryLock);
}

/** Clears all storage and resets the revision. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    g_slots = {};
    g_revision = 0;
    ReleaseSRWLockExclusive(&g_registryLock);
}

namespace {

// Separate from the registry lock: a slot calls into the registry while holding this one.
SRWLOCK g_slotLock{SRWLOCK_INIT};

} // namespace

/** Registers the page once, building its descriptor from the supplied identity. */
bool PageRegistration::acquire(Owner owner,
                               std::string_view stableId,
                               std::string_view displayName,
                               FrameCallback callback,
                               void (*prepare)() noexcept,
                               FrameCallback companionCallback,
                               FrameCallback deactivate,
                               Presentation presentation) noexcept {
    AcquireSRWLockExclusive(&g_slotLock);
    if (registered_) {
        ReleaseSRWLockExclusive(&g_slotLock);
        return true;
    }
    if (prepare != nullptr) {
        prepare();
    }
    Descriptor descriptor;
    if (create_descriptor(owner, stableId, displayName, callback, companionCallback, descriptor)
        && register_module(descriptor.with_deactivation(deactivate).with_presentation(presentation))
               == RegistrationResult::registered) {
        stableId_ = stableId;
        registered_ = true;
    }
    const bool registered = registered_;
    ReleaseSRWLockExclusive(&g_slotLock);
    return registered;
}

/** Removes the page when the slot holds one. */
void PageRegistration::release(void (*finish)() noexcept) noexcept {
    AcquireSRWLockExclusive(&g_slotLock);
    if (registered_) {
        (void)unregister_module(stableId_);
        stableId_ = {};
        registered_ = false;
    }
    if (finish != nullptr) {
        finish();
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

} // namespace sunrise::core::ui::modules::registry
