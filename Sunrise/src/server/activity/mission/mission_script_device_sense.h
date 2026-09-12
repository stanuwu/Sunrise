#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Type-23 Sense reports current position, power and lock, each followed by its sequence. */
inline constexpr std::uint32_t kDeviceSenseSchema = 0x80804F47U;
inline constexpr std::uint8_t kDeviceSenseSlotType = 23;
inline constexpr std::size_t kDeviceSenseChannelCount =
    middleware::bap::activity_message::scriptable_auth::kType23ChannelCount;

/** A sequence of -1 is the native device's initial value. */
struct DeviceChannelLevel final {
    float value{};
    std::int32_t sequence{-1};
    bool valueKnown{};
    bool sequenceKnown{};
    bool operator==(const DeviceChannelLevel&) const = default;
};

/** Only reported fields are known; an absent optional field retains its previous value. */
struct DeviceLevel final {
    std::array<DeviceChannelLevel, kDeviceSenseChannelCount> channels{};
    bool observed{};
    bool operator==(const DeviceLevel&) const = default;
};

/**
 * Merges accepted current values without substituting a command's target value.
 * @param retained Last reported levels for this exact device and client generation.
 * @param values Accepted Type-23 Sense fields.
 * @param root Native schema identity.
 * @param reset True when a reported sequence regressed and cleared prior observations.
 * @return True when a level changed; invalid input leaves retained state unchanged.
 */
[[nodiscard]] inline bool update_device_level(
    DeviceLevel& retained,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root,
    bool& reset) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    DeviceLevel incoming{};
    reset = false;
    bool sequenceRegressed = false;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != root || value.occurrence != 0
            || value.fieldOrdinal >= 2 * kDeviceSenseChannelCount) {
            continue;
        }
        const auto index = value.fieldOrdinal / 2;
        auto& channel = incoming.channels[index];
        if (value.fieldOrdinal % 2 == 0) {
            if (value.kind != sense::ValueKind::real32 || !std::isfinite(value.realValue)
                || value.realValue < 0.0F || value.realValue > 1.0F || channel.valueKnown) {
                return false;
            }
            channel.value = value.realValue;
            channel.valueKnown = true;
        } else {
            if (value.kind != sense::ValueKind::signedInteger || value.signedValue < -1
                || value.signedValue > (std::numeric_limits<std::int32_t>::max)()
                || channel.sequenceKnown) {
                return false;
            }
            channel.sequence = static_cast<std::int32_t>(value.signedValue);
            channel.sequenceKnown = true;
            sequenceRegressed = sequenceRegressed
                                || (retained.channels[index].sequenceKnown
                                    && channel.sequence < retained.channels[index].sequence);
        }
        incoming.observed = true;
    }
    if (!incoming.observed) {
        return false;
    }
    DeviceLevel next = sequenceRegressed ? DeviceLevel{} : retained;
    for (std::size_t index = 0; index < next.channels.size(); ++index) {
        if (incoming.channels[index].valueKnown) {
            next.channels[index].value = incoming.channels[index].value;
            next.channels[index].valueKnown = true;
        }
        if (incoming.channels[index].sequenceKnown) {
            next.channels[index].sequence = incoming.channels[index].sequence;
            next.channels[index].sequenceKnown = true;
        }
    }
    next.observed = true;
    const bool changed = next != retained;
    retained = next;
    reset = sequenceRegressed;
    return changed;
}

} // namespace sunrise::server::activity::mission
