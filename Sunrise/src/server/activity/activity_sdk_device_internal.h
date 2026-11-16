#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../state/build_data/runtime.h"
#include "../bap/runtime.h"
#include "activity_sdk_device_runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_devices::detail {

/** Exact private route retained between preflight and queueing. */
struct PreparedDevice final {
    host::ScriptableTarget target{};
    state::build_data::scenarios::RosterGroup generatedRosterGroup{};
    std::uint64_t activityClientGeneration{};
    std::uint32_t scenarioRow{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t stateRow{state::activity_sdk::format::kAbsentIndex};
    std::int32_t effectiveRegion{-1};
};

/** Maps the SDK binding validator to this API's stable refusal surface. */
[[nodiscard]] Status binding_status(const state::activity_sdk::BoundView& view,
                                    server::bap::ActivityLinkView& link) noexcept;

/**
 * Resolves one SDK slot through a published canonical group or its exact occurrence: the one in
 * the client's region, else the one outside the client's bubble.
 */
[[nodiscard]] Status prepare_slot(const state::activity_sdk::BoundView& view,
                                  std::uint32_t slotRow,
                                  PreparedDevice& output) noexcept;

/** Resolves the legacy type-23 device facade through the shared typed SDK Auth slot route. */
[[nodiscard]] Status
prepare(const state::activity_sdk::BoundView& view,
        std::uint32_t slotRow,
        middleware::bap::activity_message::scriptable_auth::Type23Channel channel,
        float value,
        PreparedDevice& output) noexcept;

/**
 * Resolves one exact type-31 trigger slot. Type 31 carries no caller value, so unlike the device
 * path there is no channel or range to check first.
 * @param view Pinned SDK view whose binding is revalidated.
 * @param slotRow Catalog row the pulse targets.
 * @param output Cleared, then receives the resolved target.
 * @return ready only for an exact type-31 slot on a live owned binding.
 */
[[nodiscard]] Status prepare_trigger(const state::activity_sdk::BoundView& view,
                                     std::uint32_t slotRow,
                                     PreparedDevice& output) noexcept;

/** Resolves one package-owned object entry without accepting a caller transform. */
[[nodiscard]] Status prepare_object(const state::activity_sdk::BoundView& view,
                                    std::uint32_t slotRow,
                                    std::int32_t entryIndex,
                                    PreparedDevice& output) noexcept;

/** Resolves one exact actor channel bridge. */
[[nodiscard]] Status prepare_combatant(const state::activity_sdk::BoundView& view,
                                       std::uint32_t slotRow,
                                       PreparedDevice& output) noexcept;

/**
 * Verifies exact SDK identity and schema-decodes one retained Auth body.
 * @param body Encoded Auth body; its bit count must match one accepted schema exactly.
 * @param sdkBuildSha256 Caller's SDK build digest, compared against the bound catalog.
 * @return ready only when identity, slot, padding and body shape all hold.
 */
[[nodiscard]] Status validate_auth(const state::activity_sdk::BoundView& view,
                                   std::uint32_t slotRow,
                                   std::uint32_t objectTag,
                                   std::uint32_t registryKey,
                                   std::uint32_t authSchema,
                                   std::uint16_t slotIndex,
                                   std::uint8_t slotType,
                                   std::span<const std::byte> body,
                                   std::uint16_t bitCount,
                                   std::span<const std::byte> sdkBuildSha256) noexcept;

} // namespace sunrise::server::activity::activity_sdk_devices::detail
