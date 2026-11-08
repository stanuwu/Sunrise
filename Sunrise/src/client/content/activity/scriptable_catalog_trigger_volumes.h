#pragma once

#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::client::content::activity::scriptables::internal {

/** One placed-object config reached while analyzing one exact scenario object occurrence. */
struct TriggerVolumeInput final {
    std::uint32_t objectRow{};
    std::uint32_t configTag{};
};

using TriggerVolumeCancelCheck = bool (*)() noexcept;

/** Appends the slot-owned trigger volumes of every reached package config. */
[[nodiscard]] bool
append_trigger_volumes(const middleware::content::packages::reader::Source& source,
                       middleware::content::packages::reader::Scratch& scratch,
                       std::span<const TriggerVolumeInput> inputs,
                       state::build_data::scriptables::Snapshot& output,
                       TriggerVolumeCancelCheck cancel) noexcept;

} // namespace sunrise::client::content::activity::scriptables::internal
