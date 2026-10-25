#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sunrise::client::content::activity::sdk_generation::incident_inventory {

/** One English incident text fits this storage including its terminator. */
inline constexpr std::size_t kTextCapacity = 256U;

/** One owned string retained until the final pack string table is linked. */
struct Text final {
    std::array<char, kTextCapacity> value{};
    std::uint16_t length{};
};

/** One notification row of the SObject table and the English text its name hash keys. */
struct Definition final {
    Text text{};
    /** SObject row, the incident's primary target. */
    std::uint32_t target{};
    std::uint32_t nameHash{};
    /** First installed string container, by tag, that holds the name hash. */
    std::uint32_t containerTag{};
    std::int32_t typeCode{};
};

/** Every incident notification the installed content names, in target order. */
struct Snapshot final {
    std::vector<Definition> definitions{};
    /** SObject table the rows were read from; zero when no installed table was readable. */
    std::uint32_t tableTag{};
    /** Notification rows in the table, with or without an English text. */
    std::uint32_t notificationRows{};
};

} // namespace sunrise::client::content::activity::sdk_generation::incident_inventory
