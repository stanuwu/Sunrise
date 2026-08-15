#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../ui_module_descriptor.h"

namespace sunrise::core::ui::modules::registry {

/** 32 slots cover every UI domain and cap the static registry storage. */
inline constexpr std::size_t kModuleCapacity = 32;

/** Registration outcomes keep the duplicate, invalid and full cases apart. */
enum class RegistrationResult : std::uint8_t {
    registered,
    invalidDescriptor,
    duplicateStableId,
    capacityReached,
};

/** A copy of the module list, taken under one shared registry lock. */
class RegistrySnapshot final {
public:
    /** @return Every descriptor, in menu order. */
    [[nodiscard]] std::span<const Descriptor> entries() const noexcept;

    /** @return Registry revision captured with the descriptors. */
    [[nodiscard]] std::uint64_t revision() const noexcept;

    /** @return A copy of every module, taken under the lock, in menu order. */
private:
    friend RegistrySnapshot snapshot() noexcept;

    std::array<Descriptor, kModuleCapacity> entries_{};
    std::size_t count_{};
    std::uint64_t revision_{};
};

/**
 * Copies one descriptor into the fixed registry.
 * @return The outcome. Nothing changes when the descriptor is rejected.
 */
[[nodiscard]] RegistrationResult register_module(const Descriptor& descriptor) noexcept;

/**
 * Removes one module by its globally unique stable ID.
 * @param stableId Exact registered ID.
 * @return True when one registration was removed.
 */
[[nodiscard]] bool unregister_module(std::string_view stableId) noexcept;

/**
 * Copies every registered module. The menu is one list, so the order is by owner: Client,
 * then Server, then Core.
 * @return A copy, taken under the lock.
 */
[[nodiscard]] RegistrySnapshot snapshot() noexcept;

/**
 * One page's registration slot, owned by the part that supplies the page. Each part holds its
 * own slot instead of calling the registry, so registering twice does nothing and shutdown is
 * its exact reverse. Core hands the slot to Client, Server and its own pages, not connecting them.
 */
class PageRegistration final {
public:
    /**
     * Registers the page once, building its descriptor from the supplied identity.
     * @param owner Part supplying the page. It fixes the menu order.
     * @param stableId Namespaced id, unique across every part.
     * @param displayName Menu label.
     * @param callback Module frame entry.
     * @param prepare Optional state reset, run under the slot lock before registering.
     * @return True when the slot holds a registration afterwards.
     */
    [[nodiscard]] bool acquire(Owner owner,
                               std::string_view stableId,
                               std::string_view displayName,
                               FrameCallback callback,
                               void (*prepare)() noexcept = nullptr) noexcept;

    /**
     * Removes the page when the slot holds one.
     * @param finish Optional state reset, run under the slot lock after unregistering.
     */
    void release(void (*finish)() noexcept = nullptr) noexcept;

private:
    // Guarded by a lock defined in the .cpp, which keeps Windows.h out of this header. Slots are
    // few and only touched at part startup and shutdown, so one lock covers all of them.
    bool registered_{};
    std::string_view stableId_{};
};

/** @param owner Domain whose registrations are removed as one locked mutation. */
void clear(Owner owner) noexcept;

/** Clears every registration. The revision keeps rising. */
void clear() noexcept;

/** Clears all storage and resets the revision. */
void shutdown() noexcept;

} // namespace sunrise::core::ui::modules::registry
