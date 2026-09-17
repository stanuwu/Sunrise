#include "public_profile_codec.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <type_traits>

#include "../../state/account/public_profiles.h"
#include "../encoding/bit_reader.h"
#include "../encoding/bit_writer.h"

namespace sunrise::middleware::profile {
namespace {
/** ASCII "SPRF": the public-profile container marker, never a value the game client produces. */
constexpr std::uint32_t kMagic = 0x53505246;
/** Version 6 removes the two join-lock fields; older layouts must be refused before decoding. */
constexpr std::uint16_t kVersion = 6;

class Encoder {
public:
    explicit Encoder(std::span<std::byte> output) noexcept : bits_(output) {}
    template <typename T> bool field(const T& value) noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            return bits_.write(value ? 1 : 0, 8);
        } else if constexpr (std::is_enum_v<T>) {
            return field(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, float>) {
            return field(std::bit_cast<std::uint32_t>(value));
        } else {
            static_assert(std::is_integral_v<T>);
            return bits_.write(static_cast<std::uint64_t>(value), sizeof(T) * 8);
        }
    }
    template <typename... T> bool values(const T&... value) noexcept {
        return (field(value) && ...);
    }
    template <std::size_t N> bool text(const std::array<char, N>& value) noexcept {
        for (const auto byte : value) {
            if (!field(byte)) {
                return false;
            }
        }
        return true;
    }
    bool count(std::size_t value, std::size_t maximum) noexcept {
        return value <= maximum && value <= 0xFFFFU && bits_.write(value, 16);
    }
    template <typename T, typename Visit>
    bool optional(const std::optional<T>& value, Visit visit) noexcept {
        return field(value.has_value()) && (!value.has_value() || visit(*value));
    }
    bool finish(std::size_t& written) noexcept {
        return bits_.finish(written);
    }

private:
    encoding::bits::Writer bits_;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::byte> input) noexcept : bits_(input) {}
    template <typename T> bool field(T& value) noexcept {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint64_t raw{};
            if (!bits_.read(8, raw) || raw > 1) {
                return false;
            }
            value = raw != 0;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            if (!field(raw)) {
                return false;
            }
            value = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, float>) {
            std::uint32_t raw{};
            if (!field(raw)) {
                return false;
            }
            value = std::bit_cast<float>(raw);
        } else {
            static_assert(std::is_integral_v<T>);
            std::uint64_t raw{};
            if (!bits_.read(sizeof(T) * 8, raw)) {
                return false;
            }
            value = std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(raw));
        }
        return true;
    }
    template <typename... T> bool values(T&... value) noexcept {
        return (field(value) && ...);
    }
    template <std::size_t N> bool text(std::array<char, N>& value) noexcept {
        for (auto& byte : value) {
            if (!field(byte)) {
                return false;
            }
        }
        return true;
    }
    bool count(std::size_t& value, std::size_t maximum) noexcept {
        std::uint64_t raw{};
        if (!bits_.read(16, raw) || raw > maximum) {
            return false;
        }
        value = static_cast<std::size_t>(raw);
        return true;
    }
    template <typename T, typename Visit>
    bool optional(std::optional<T>& value, Visit visit) noexcept {
        bool present{};
        if (!field(present)) {
            return false;
        }
        if (!present) {
            value.reset();
            return true;
        }
        value.emplace();
        return visit(*value);
    }
    bool complete() const noexcept {
        return bits_.remaining_bits() == 0;
    }

private:
    encoding::bits::Reader bits_;
};

template <typename Archive, typename Item> bool item_fields(Archive& archive, Item& item) noexcept {
    // New-item receipts and mutation counters belong only to the owner's inventory.
    if constexpr (!std::is_const_v<Item>) {
        item.seen = true;
        item.mutationSerial = 0;
    }
    if (!archive.values(item.instanceSoid,
                        item.definitionHash,
                        item.level,
                        item.quantity,
                        item.flags,
                        item.movementAbilityEntry,
                        item.grenadeAbilityEntry,
                        item.superAbilityEntry,
                        item.meleeAbilityEntry,
                        item.classAbilityEntry,
                        item.sockets.policy)
        || !archive.count(item.sockets.plugCount, item.sockets.plugs.size())) {
        return false;
    }
    for (std::size_t i = 0; i < item.sockets.plugCount; ++i) {
        if (!archive.optional(item.sockets.plugs[i],
                              [&](auto& value) { return archive.field(value); })) {
            return false;
        }
    }
    return true;
}

template <typename Archive, typename Character>
bool character_fields(Archive& archive, Character& character) noexcept {
    if (!archive.values(character.soid,
                        character.selected,
                        character.race,
                        character.gender,
                        character.characterClass,
                        character.level,
                        character.previewAvailable,
                        character.appearanceValue,
                        character.lastOrbitedDestination,
                        character.currentActivityIndex,
                        character.contentBypass,
                        character.equippedTitleRecordIndex,
                        character.signInSeconds,
                        character.acquiredSubclassAbilityMask)) {
        return false;
    }
    for (auto& item : character.equipment.slots) {
        if (!archive.optional(item, [&](auto& value) { return item_fields(archive, value); })) {
            return false;
        }
    }
    // Only equipment is public; the private bag has no field in this layout.
    return true;
}

template <typename Archive, typename Presence>
bool native_presence_fields(Archive& archive, Presence& presence) noexcept {
    if (!archive.values(presence.published,
                        presence.characterSoid,
                        presence.hasGroup,
                        presence.groupKey,
                        presence.memberCount,
                        presence.hasFireteam,
                        presence.descriptorSize)
        || presence.descriptorSize > presence.descriptor.size()) {
        return false;
    }
    if (presence.hasFireteam) {
        for (auto& byte : presence.fireteam) {
            if (!archive.field(byte)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < presence.descriptorSize; ++i) {
        if (!archive.field(presence.descriptor[i])) {
            return false;
        }
    }
    return true;
}

template <typename Archive, typename Account>
bool account_fields(Archive& archive, Account& account) noexcept {
    auto& presence = account.presence;
    if (!archive.values(account.primarySoid, presence.platformId, presence.flags)
        || !archive.text(presence.displayName) || !archive.text(presence.personaName)
        || !archive.text(presence.nameCode)
        || !archive.count(account.characterCount, account.characters.size())) {
        return false;
    }
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        if (!character_fields(archive, account.characters[i])) {
            return false;
        }
    }
    return archive.field(presence.artifactPowerBonus)
           && native_presence_fields(archive, presence.native);
}

} // namespace

bool valid(const state::AccountState& account) noexcept {
    return state::account::profiles::valid(account);
}

bool encode(const state::AccountState& account,
            std::span<std::byte> output,
            std::size_t& written) noexcept {
    written = 0;
    if (!valid(account)) {
        return false;
    }
    Encoder archive(output.first((std::min)(output.size(), kMaximumEncodedSize)));
    return archive.values(kMagic, kVersion) && account_fields(archive, account)
           && archive.finish(written);
}

bool decode(std::span<const std::byte> input,
            state::AccountState& output,
            state::AccountState& staging) noexcept {
    if (input.size() > kMaximumEncodedSize || &output == &staging) {
        return false;
    }
    // Reinitialize the caller's storage directly; no account-sized stack temporary is needed.
    std::destroy_at(&staging);
    std::construct_at(&staging);
    Decoder archive(input);
    std::uint32_t magic{};
    std::uint16_t version{};
    if (!archive.values(magic, version) || magic != kMagic || version != kVersion
        || !account_fields(archive, staging) || !archive.complete() || !valid(staging)) {
        return false;
    }
    output = staging;
    return true;
}

} // namespace sunrise::middleware::profile
