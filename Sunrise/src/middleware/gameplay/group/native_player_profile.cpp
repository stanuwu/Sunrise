#include "native_player_profile.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace sunrise::middleware::gameplay::group {
namespace {
constexpr std::uint16_t kExtraFields = 0x1DC;
} // namespace

bool read_native_player_profile(encoding::bits::Reader& reader,
                                NativePlayerProfile& output) noexcept {
    output = {};
    NativePlayerProfile profile;
    std::uint64_t present{}, value{};
    if (!reader.read(1, present)) {
        return false;
    }
    profile.hasName = present != 0;
    if (profile.hasName) {
        while (profile.nameLength < profile.name.size()) {
            if (!reader.read(16, value)) {
                return false;
            }
            if (!value) {
                break;
            }
            profile.name[profile.nameLength++] = static_cast<char16_t>(value);
        }
    }
    if (!reader.read(1, present)) {
        return false;
    }
    profile.hasIdentity = present != 0;
    if (profile.hasIdentity) {
        for (auto& byte : profile.identity) {
            if (!reader.read(8, value)) {
                return false;
            }
            byte = static_cast<std::byte>(value);
        }
    }
    auto field = [&](std::uint16_t mask, std::uint8_t width, auto& destination) {
        if (!reader.read(1, present)) {
            return false;
        }
        if (!present) {
            return true;
        }
        if (!reader.read(width, value)) {
            return false;
        }
        profile.fields |= mask;
        destination = static_cast<std::remove_reference_t<decltype(destination)>>(value);
        return true;
    };
    if (!field(4, 8, profile.q3) || !field(8, 6, profile.q4) || !field(0x100, 6, profile.q5)
        || !reader.read(1, present)) {
        return false;
    }
    if (present) {
        profile.fields |= 0x10;
        for (auto& word : profile.q6) {
            if (!reader.read(16, value)) {
                return false;
            }
            word = static_cast<std::int16_t>(value);
        }
    }
    if (!reader.read(1, present)) {
        return false;
    }
    profile.soids.present = present != 0;
    if (profile.soids.present
        && (!reader.read(64, profile.soids.accountSoid)
            || !reader.read(64, profile.soids.characterSoid))) {
        return false;
    }
    if (!reader.read(1, present)) {
        return false;
    }
    if (present) {
        profile.fields |= 0x40;
        if (!reader.read(32, value)) {
            return false;
        }
        profile.q8Handle = static_cast<std::uint32_t>(value);
        for (auto& slot : profile.q8Slots) {
            if (!reader.read(16, value)) {
                return false;
            }
            slot = static_cast<std::uint16_t>(value ^ 0x8000U);
        }
        if (!reader.read(8, value)) {
            return false;
        }
        profile.q8Tail = static_cast<std::uint8_t>(value);
    }
    if (!field(0x80, 32, profile.q9) || !valid_native_player_profile(profile)) {
        return false;
    }
    output = profile;
    return true;
}

bool valid_native_player_profile(const NativePlayerProfile& profile) noexcept {
    if (profile.nameLength > profile.name.size() || (!profile.hasName && profile.nameLength)) {
        return false;
    }
    for (std::size_t i = 0; i < profile.nameLength; ++i) {
        if (!profile.name[i]) {
            return false;
        }
    }
    return !(profile.fields & ~kExtraFields) && profile.q4 <= 32 && profile.q5 <= 32
           && profile.tailFlags <= 31 && profile.tailKind <= 3
           && (!(profile.tailFlags & 0x10) || profile.tailIndex <= 31
               || profile.tailIndex == UINT32_MAX);
}

bool write_native_player_profile(encoding::bits::Writer& writer,
                                 const NativePlayerProfile& profile) noexcept {
    if (!valid_native_player_profile(profile) || !writer.write(profile.hasName ? 1 : 0, 1)) {
        return false;
    }
    if (profile.hasName) {
        for (std::size_t i = 0; i < profile.nameLength; ++i) {
            if (!writer.write(profile.name[i], 16)) {
                return false;
            }
        }
        if (profile.nameLength < profile.name.size() && !writer.write(0, 16)) {
            return false;
        }
    }
    if (!writer.write(profile.hasIdentity ? 1 : 0, 1)) {
        return false;
    }
    if (profile.hasIdentity) {
        for (const auto byte : profile.identity) {
            if (!writer.write(std::to_integer<std::uint8_t>(byte), 8)) {
                return false;
            }
        }
    }
    auto field = [&](std::uint16_t mask, std::uint8_t width, std::uint64_t value) {
        return writer.write((profile.fields & mask) ? 1 : 0, 1)
               && (!(profile.fields & mask) || writer.write(value, width));
    };
    if (!field(4, 8, profile.q3) || !field(8, 6, profile.q4) || !field(0x100, 6, profile.q5)
        || !writer.write((profile.fields & 0x10) ? 1 : 0, 1)) {
        return false;
    }
    if (profile.fields & 0x10) {
        for (const auto word : profile.q6) {
            if (!writer.write(static_cast<std::uint16_t>(word), 16)) {
                return false;
            }
        }
    }
    if (!writer.write(profile.soids.present ? 1 : 0, 1)) {
        return false;
    }
    if (profile.soids.present
        && (!writer.write(profile.soids.accountSoid, 64)
            || !writer.write(profile.soids.characterSoid, 64))) {
        return false;
    }
    if (!writer.write((profile.fields & 0x40) ? 1 : 0, 1)) {
        return false;
    }
    if (profile.fields & 0x40) {
        if (!writer.write(profile.q8Handle, 32)) {
            return false;
        }
        for (const auto slot : profile.q8Slots) {
            if (!writer.write(slot ^ 0x8000U, 16)) {
                return false;
            }
        }
        if (!writer.write(profile.q8Tail, 8)) {
            return false;
        }
    }
    return field(0x80, 32, profile.q9);
}

bool complete_native_player_profile(const NativePlayerProfile& profile) noexcept {
    return valid_native_player_profile(profile) && profile.hasName && profile.hasIdentity
           && profile.soids.present && profile.fields == kExtraFields && profile.hasTail;
}

bool read_native_player_tail(encoding::bits::Reader& reader,
                             NativePlayerProfile& profile) noexcept {
    auto candidate = profile;
    std::uint64_t value{};
    for (auto& word : candidate.tailWords) {
        if (!reader.read(32, value)) {
            return false;
        }
        word = static_cast<std::uint32_t>(value);
    }
    if (!reader.read(5, value)) {
        return false;
    }
    candidate.tailFlags = static_cast<std::uint8_t>(value);
    if (!reader.read(2, value)) {
        return false;
    }
    candidate.tailKind = static_cast<std::uint8_t>(value);
    if (!reader.read(1, value)) {
        return false;
    }
    candidate.tailFlag = value != 0;
    candidate.tailIndex = 0;
    if (candidate.tailFlags & 0x10) {
        if (!reader.read(1, value)) {
            return false;
        }
        if (value) {
            candidate.tailIndex = UINT32_MAX;
        } else {
            if (!reader.read(5, value)) {
                return false;
            }
            candidate.tailIndex = static_cast<std::uint32_t>(value);
        }
    }
    candidate.hasTail = true;
    profile = candidate;
    return true;
}

bool write_native_player_tail(encoding::bits::Writer& writer,
                              const NativePlayerProfile& profile) noexcept {
    if (!profile.hasTail || !valid_native_player_profile(profile)) {
        return false;
    }
    for (const auto word : profile.tailWords) {
        if (!writer.write(word, 32)) {
            return false;
        }
    }
    if (!writer.write(profile.tailFlags, 5) || !writer.write(profile.tailKind, 2)
        || !writer.write(profile.tailFlag ? 1 : 0, 1)) {
        return false;
    }
    return !(profile.tailFlags & 0x10)
           || (writer.write(profile.tailIndex == UINT32_MAX ? 1 : 0, 1)
               && (profile.tailIndex == UINT32_MAX || writer.write(profile.tailIndex, 5)));
}

void merge_native_player_profile(NativePlayerProfile& target,
                                 const NativePlayerProfile& update) noexcept {
    if (update.hasName) {
        target.hasName = true;
        target.name = update.name;
        target.nameLength = update.nameLength;
    }
    if (update.hasIdentity) {
        target.hasIdentity = true;
        target.identity = update.identity;
    }
    if (update.soids.present) {
        target.soids = update.soids;
    }
    if (update.fields & 4) {
        target.q3 = update.q3;
    }
    if (update.fields & 8) {
        target.q4 = update.q4;
    }
    if (update.fields & 0x100) {
        target.q5 = update.q5;
    }
    if (update.fields & 0x10) {
        target.q6 = update.q6;
    }
    if (update.fields & 0x40) {
        target.q8Handle = update.q8Handle;
        target.q8Slots = update.q8Slots;
        target.q8Tail = update.q8Tail;
    }
    if (update.fields & 0x80) {
        target.q9 = update.q9;
    }
    target.fields |= update.fields;
    if (update.hasTail) {
        target.hasTail = true;
        target.tailWords = update.tailWords;
        target.tailFlags = update.tailFlags;
        target.tailKind = update.tailKind;
        target.tailFlag = update.tailFlag;
        target.tailIndex = update.tailIndex;
    }
}

void build_native_player_profile_state(const NativePlayerProfile& profile,
                                       NativePlayerProfileState& output) noexcept {
    output = {};
    auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            output[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
        }
    };
    for (std::size_t i = 0; i <= profile.nameLength && i < profile.name.size(); ++i) {
        const auto rotation = static_cast<int>(i % 31);
        const std::uint32_t key = i ? std::rotl(std::uint32_t{0xC245B0C4}, rotation) : 0;
        const auto unit = i < profile.nameLength ? profile.name[i] : char16_t{};
        put(i * 2, (unit * 0x7B4FU) ^ key, 2);
    }
    std::copy(profile.identity.begin(), profile.identity.end(), output.begin() + 0x80);
    put(0xB0, profile.q3, 2);
    put(0xB2, static_cast<std::uint8_t>(profile.q4 - 1), 1);
    put(0xB3, static_cast<std::uint8_t>(profile.q5 - 1), 1);
    put(0xB8, static_cast<std::uint32_t>(profile.q6[0]), 4);
    put(0xBC, static_cast<std::uint32_t>(profile.q6[1]), 4);
    put(0xC0, profile.soids.accountSoid, 8);
    put(0xC8, profile.soids.characterSoid, 8);
    put(0xD0, profile.q8Handle, 4);
    for (std::size_t i = 0; i < profile.q8Slots.size(); ++i) {
        put(0xD4 + i * 2, profile.q8Slots[i], 2);
    }
    put(0xDC, profile.q8Tail, 1);
    put(0xE0, profile.q9, 4);
}
} // namespace sunrise::middleware::gameplay::group
