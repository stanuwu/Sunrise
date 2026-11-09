#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808064.h"

namespace sunrise::middleware::bap::activity_message::kill_incident {

namespace generated = middleware::content::packages::schema::generated;

/** Runtime metadata schema carried by the kill incident payload. */
inline constexpr std::uint32_t kSchema = generated::C80806442::kClass;
/** C80806442 ends with three label lists of up to fifteen name hashes each. */
inline constexpr std::size_t kLabelCapacity = sizeof(generated::C80806448) / sizeof(std::uint32_t);

/** One side of a kill, from its C80806446 actor record. */
struct Actor final {
    /** Zero when the payload names no player. */
    std::uint64_t playerKey{};
    /** Name hash of the actor definition's class; zero when absent. */
    std::uint32_t classHash{};
    /** The record's first flag, which every enemy victim sets. */
    bool combatant{};
};

/** One list of label name hashes, in wire order. */
struct Labels final {
    std::array<std::uint32_t, kLabelCapacity> values{};
    std::size_t count{};

    /** @return The labels in use. */
    [[nodiscard]] std::span<const std::uint32_t> view() const noexcept {
        return std::span(values).first(count);
    }
};

/** The kill facts retained from one payload. */
struct Payload final {
    Actor killer{};
    Actor victim{};
    /** Equipment slot of the killing source, unbiased; absent when the field is omitted. */
    std::optional<std::int8_t> equipmentSlot{};
    /** Ability selector of the killing source, unbiased; -1 names no ability. */
    std::optional<std::int8_t> abilitySelector{};
    /** Damage type of the final blow, unbiased; -1 when the event carries none. */
    std::int8_t damageType{-1};
    /** Labels of the killing source, the killing actor and the victim. */
    Labels source{};
    Labels actor{};
    Labels target{};
};

/** Decodes one kSchema payload, including its optional fields and required zero padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Payload& output) noexcept;

} // namespace sunrise::middleware::bap::activity_message::kill_incident
