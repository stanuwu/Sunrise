#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "../filesystem/path.h"
#include "../logging/log.h"
#include "settings.h"

namespace sunrise::core::settings::persistence {
namespace detail {

constexpr std::wstring_view kSettingsFileSuffix = L"\\settings.json";
constexpr std::wstring_view kStageFileSuffix = L".new";
constexpr std::size_t kSettingsCapacity = 1024 * 1024;
constexpr std::size_t kRewriteCapacity = kSettingsCapacity + 32768;

/** Module-local address used only to resolve Sunrise's own settings location. */
inline const std::byte g_moduleAnchor{};
/** Settings replacement is process-global even though creator requests normally serialize. */
inline SRWLOCK g_storeLock{SRWLOCK_INIT};

[[nodiscard]] inline HMODULE module_handle() noexcept {
    HMODULE module = nullptr;
    const auto moduleAddress = reinterpret_cast<LPCWSTR>(&g_moduleAnchor);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           moduleAddress,
                           &module)
        == FALSE) {
        return nullptr;
    }
    return module;
}

[[nodiscard]] inline bool settings_path(path::Buffer& output) noexcept {
    const HMODULE module = module_handle();
    return module != nullptr && path::artifact_directory(module, output)
           && path::append(output, kSettingsFileSuffix);
}

[[nodiscard]] inline bool whitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] inline std::size_t skip_whitespace(std::string_view text,
                                                 std::size_t position) noexcept {
    while (position < text.size() && whitespace(text[position])) {
        ++position;
    }
    return position;
}

[[nodiscard]] inline bool scan_string(std::string_view text,
                                      std::size_t begin,
                                      std::size_t& end) noexcept {
    end = 0;
    if (begin >= text.size() || text[begin] != '"') {
        return false;
    }
    std::size_t position = begin + 1U;
    while (position < text.size()) {
        if (text[position] == '\\') {
            position += 2U;
            continue;
        }
        if (text[position] == '"') {
            end = position + 1U;
            return true;
        }
        ++position;
    }
    return false;
}

[[nodiscard]] inline bool scan_value(std::string_view text,
                                     std::size_t begin,
                                     std::size_t& end) noexcept {
    end = 0;
    begin = skip_whitespace(text, begin);
    if (begin >= text.size()) {
        return false;
    }
    if (text[begin] == '"') {
        return scan_string(text, begin, end);
    }
    if (text[begin] == '{' || text[begin] == '[') {
        std::size_t objectDepth = 0;
        std::size_t arrayDepth = 0;
        std::size_t position = begin;
        while (position < text.size()) {
            if (text[position] == '"') {
                if (!scan_string(text, position, position)) {
                    return false;
                }
                continue;
            }
            if (text[position] == '{') {
                ++objectDepth;
            } else if (text[position] == '}') {
                if (objectDepth == 0) {
                    return false;
                }
                --objectDepth;
            } else if (text[position] == '[') {
                ++arrayDepth;
            } else if (text[position] == ']') {
                if (arrayDepth == 0) {
                    return false;
                }
                --arrayDepth;
            }
            ++position;
            if (objectDepth == 0 && arrayDepth == 0) {
                end = position;
                return true;
            }
        }
        return false;
    }

    std::size_t position = begin;
    while (position < text.size() && !whitespace(text[position]) && text[position] != ','
           && text[position] != '}' && text[position] != ']') {
        ++position;
    }
    if (position == begin) {
        return false;
    }
    end = position;
    return true;
}

struct MemberSpan {
    std::size_t valueBegin{};
    std::size_t valueEnd{};
    bool found{};
};

[[nodiscard]] inline bool key_matches(std::string_view text,
                                      std::size_t begin,
                                      std::size_t end,
                                      std::string_view target) noexcept {
    return end >= begin + 2U && text[begin] == '"' && text[end - 1U] == '"'
           && text.substr(begin + 1U, end - begin - 2U) == target;
}

[[nodiscard]] inline bool find_object_member(std::string_view text,
                                             std::size_t objectBegin,
                                             std::size_t objectEnd,
                                             std::string_view target,
                                             MemberSpan& output) noexcept {
    output = {};
    if (objectBegin >= objectEnd || objectEnd > text.size() || text[objectBegin] != '{'
        || text[objectEnd - 1U] != '}') {
        return false;
    }
    std::size_t position = skip_whitespace(text, objectBegin + 1U);
    if (position < objectEnd && text[position] == '}') {
        return true;
    }
    while (position < objectEnd) {
        const std::size_t keyBegin = position;
        std::size_t keyEnd = 0;
        if (!scan_string(text, keyBegin, keyEnd)) {
            return false;
        }
        position = skip_whitespace(text, keyEnd);
        if (position >= objectEnd || text[position] != ':') {
            return false;
        }
        const std::size_t valueBegin = skip_whitespace(text, position + 1U);
        std::size_t valueEnd = 0;
        if (!scan_value(text, valueBegin, valueEnd) || valueEnd > objectEnd) {
            return false;
        }
        if (key_matches(text, keyBegin, keyEnd, target)) {
            output.valueBegin = valueBegin;
            output.valueEnd = valueEnd;
            output.found = true;
            return true;
        }
        position = skip_whitespace(text, valueEnd);
        if (position >= objectEnd) {
            return false;
        }
        if (text[position] == '}') {
            return true;
        }
        if (text[position] != ',') {
            return false;
        }
        position = skip_whitespace(text, position + 1U);
    }
    return false;
}

class Writer final {
public:
    explicit Writer(std::span<char> storage) noexcept : storage_(storage) {}

    [[nodiscard]] bool append(std::string_view text) noexcept {
        if (!ok_ || text.size() > storage_.size() - size_) {
            ok_ = false;
            return false;
        }
        std::copy(text.begin(), text.end(), storage_.data() + size_);
        size_ += text.size();
        return true;
    }

    [[nodiscard]] bool indent(std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (!append(" ")) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool append_uint(std::uint64_t value) noexcept {
        std::array<char, 24> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (result.ec != std::errc{}) {
            ok_ = false;
            return false;
        }
        return append({digits.data(), static_cast<std::size_t>(result.ptr - digits.data())});
    }

    [[nodiscard]] bool append_int(std::int64_t value) noexcept {
        std::array<char, 24> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        if (result.ec != std::errc{}) {
            ok_ = false;
            return false;
        }
        return append({digits.data(), static_cast<std::size_t>(result.ptr - digits.data())});
    }

    [[nodiscard]] bool append_float(float value) noexcept {
        std::array<char, 32> text{};
        const int length = std::snprintf(text.data(), text.size(), "%.9g", value);
        if (length <= 0 || static_cast<std::size_t>(length) >= text.size()) {
            ok_ = false;
            return false;
        }
        return append({text.data(), static_cast<std::size_t>(length)});
    }

    [[nodiscard]] bool append_bool(bool value) noexcept {
        return append(value ? "true" : "false");
    }

    [[nodiscard]] bool append_hex(std::uint64_t value, std::size_t digits) noexcept {
        constexpr char kHex[] = "0123456789ABCDEF";
        if (digits == 0 || digits > 16 || !append("\"0x")) {
            return false;
        }
        for (std::size_t index = 0; index < digits; ++index) {
            const std::size_t shift = (digits - index - 1U) * 4U;
            const char digit = kHex[(value >> shift) & 0xFU];
            if (!append({&digit, 1U})) {
                return false;
            }
        }
        return append("\"");
    }

    [[nodiscard]] bool append_hex_bytes(std::span<const std::byte> bytes) noexcept {
        constexpr char kHex[] = "0123456789ABCDEF";
        if (!append("\"")) {
            return false;
        }
        for (const std::byte byte : bytes) {
            const unsigned value = std::to_integer<unsigned>(byte);
            const std::array<char, 2> pair{{kHex[(value >> 4U) & 0xFU], kHex[value & 0xFU]}};
            if (!append(std::string_view(pair.data(), pair.size()))) {
                return false;
            }
        }
        return append("\"");
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::span<char> storage_;
    std::size_t size_{};
    bool ok_{true};
};

inline constexpr std::array<std::string_view, state::account::inventory::kEquipmentSlotCount>
    kEquipmentSlotNames{{"kinetic",
                         "energy",
                         "heavy",
                         "helmet",
                         "gauntlets",
                         "chest",
                         "legs",
                         "class_item",
                         "ghost",
                         "vehicle",
                         "ship",
                         "subclass",
                         "clan_banner",
                         "emblem",
                         "emote",
                         "finisher"}};

[[nodiscard]] inline bool serialize_plugs(Writer& writer,
                                          const state::account::inventory::Sockets& sockets) noexcept {
    using SocketPolicy = state::account::inventory::SocketPolicy;
    if (sockets.policy == SocketPolicy::nativeDefaults) {
        return writer.append("null");
    }
    if (sockets.policy != SocketPolicy::authored || sockets.plugCount > sockets.plugs.size()
        || !writer.append("[")) {
        return false;
    }
    for (std::size_t index = 0; index < sockets.plugCount; ++index) {
        if (index != 0 && !writer.append(", ")) {
            return false;
        }
        if (sockets.plugs[index].has_value()) {
            if (!writer.append_hex(*sockets.plugs[index], 8U)) {
                return false;
            }
        } else if (!writer.append("null")) {
            return false;
        }
    }
    return writer.append("]");
}

[[nodiscard]] inline bool serialize_item(Writer& writer,
                                         const state::account::inventory::Item& item,
                                         std::size_t indent) noexcept {
    return writer.append("{\n") && writer.indent(indent + 2U)
           && writer.append("\"instance_soid\": ") && writer.append_hex(item.instanceSoid, 16U)
           && writer.append(",\n") && writer.indent(indent + 2U)
           && writer.append("\"definition_hash\": ")
           && writer.append_hex(item.definitionHash, 8U) && writer.append(",\n")
           && writer.indent(indent + 2U) && writer.append("\"level\": ")
           && writer.append_int(item.level) && writer.append(",\n") && writer.indent(indent + 2U)
           && writer.append("\"quantity\": ") && writer.append_int(item.quantity)
           && writer.append(",\n") && writer.indent(indent + 2U) && writer.append("\"plugs\": ")
           && serialize_plugs(writer, item.sockets) && writer.append(",\n")
           && writer.indent(indent + 2U) && writer.append("\"flags\": ")
           && writer.append_uint(item.flags) && writer.append("\n") && writer.indent(indent)
           && writer.append("}");
}

[[nodiscard]] inline bool serialize_equipment(
    Writer& writer,
    const state::account::inventory::Equipment& equipment,
    std::size_t indent) noexcept {
    if (!writer.append("{")) {
        return false;
    }
    bool wrote = false;
    for (std::size_t index = 0; index < equipment.slots.size(); ++index) {
        if (!equipment.slots[index].has_value()) {
            continue;
        }
        if ((wrote && !writer.append(",")) || !writer.append("\n")
            || !writer.indent(indent + 2U) || !writer.append("\"")
            || !writer.append(kEquipmentSlotNames[index]) || !writer.append("\": ")
            || !serialize_item(writer, *equipment.slots[index], indent + 2U)) {
            return false;
        }
        wrote = true;
    }
    if (wrote && (!writer.append("\n") || !writer.indent(indent))) {
        return false;
    }
    return writer.append("}");
}

[[nodiscard]] inline bool serialize_inventory(
    Writer& writer,
    const state::account::inventory::CharacterItems& inventory,
    std::size_t indent) noexcept {
    if (inventory.count > inventory.values.size() || !writer.append("[")) {
        return false;
    }
    for (std::size_t index = 0; index < inventory.count; ++index) {
        if ((index != 0 && !writer.append(",")) || !writer.append("\n")
            || !writer.indent(indent + 2U)
            || !serialize_item(writer, inventory.values[index], indent + 2U)) {
            return false;
        }
    }
    if (inventory.count != 0 && (!writer.append("\n") || !writer.indent(indent))) {
        return false;
    }
    return writer.append("]");
}

[[nodiscard]] inline const state::account::inventory::Item*
subclass_item(const state::CharacterState& character) noexcept {
    const std::size_t index =
        static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
    if (index >= character.equipment.slots.size() || !character.equipment.slots[index].has_value()) {
        return nullptr;
    }
    return &*character.equipment.slots[index];
}

[[nodiscard]] inline bool serialize_character(const state::CharacterState& character,
                                              std::span<char> storage,
                                              std::size_t& written) noexcept {
    written = 0;
    if (character.soid == 0 || character.race > state::CharacterRace::exo
        || character.gender > state::CharacterGender::female
        || character.characterClass > state::CharacterClass::warlock
        || character.creatorTrailer > 0x1FU) {
        return false;
    }

    const state::account::inventory::Item* subclass = subclass_item(character);
    const std::uint8_t movement =
        subclass != nullptr ? subclass->movementAbilityEntry : state::kDefaultMovementAbilityEntry;
    const std::uint8_t grenade =
        subclass != nullptr ? subclass->grenadeAbilityEntry : state::kDefaultGrenadeAbilityEntry;
    const std::uint8_t superAbility =
        subclass != nullptr ? subclass->superAbilityEntry : state::kDefaultSuperAbilityEntry;
    const std::uint8_t melee =
        subclass != nullptr ? subclass->meleeAbilityEntry : state::kDefaultMeleeAbilityEntry;
    const std::uint8_t classAbility =
        subclass != nullptr ? subclass->classAbilityEntry : state::kDefaultClassAbilityEntry;

    Writer writer(storage);
    if (!writer.append("      {\n        \"soid\": ") || !writer.append_hex(character.soid, 16U)
        || !writer.append(",\n        \"race\": ")
        || !writer.append_uint(static_cast<std::uint8_t>(character.race))
        || !writer.append(",\n        \"gender\": ")
        || !writer.append_uint(static_cast<std::uint8_t>(character.gender))
        || !writer.append(",\n        \"class\": ")
        || !writer.append_uint(static_cast<std::uint8_t>(character.characterClass))
        || !writer.append(",\n        \"movement_ability\": ") || !writer.append_uint(movement)
        || !writer.append(",\n        \"grenade_ability\": ") || !writer.append_uint(grenade)
        || !writer.append(",\n        \"super_ability\": ") || !writer.append_uint(superAbility)
        || !writer.append(",\n        \"melee_ability\": ") || !writer.append_uint(melee)
        || !writer.append(",\n        \"class_ability\": ") || !writer.append_uint(classAbility)
        || !writer.append(",\n        \"level\": ") || !writer.append_uint(character.level)
        || !writer.append(",\n        \"accepted\": ") || !writer.append_bool(character.accepted)
        || !writer.append(",\n        \"preview_available\": ")
        || !writer.append_bool(character.previewAvailable)
        || !writer.append(",\n        \"appearance_value\": ")
        || !writer.append_float(character.appearanceValue)
        || !writer.append(",\n        \"last_orbited_destination\": ")
        || !writer.append_hex(character.lastOrbitedDestination, 8U)
        || !writer.append(",\n        \"content_bypass\": ") || !writer.append_bool(character.contentBypass)
        || !writer.append(",\n        \"presentation_header\": ")
        || !writer.append_hex_bytes(character.presentationHeader)
        || !writer.append(",\n        \"creation_header\": ")
        || !writer.append_hex_bytes(character.creationHeader)
        || !writer.append(",\n        \"creation_tail\": ")
        || !writer.append_hex_bytes(character.creationTail)
        || !writer.append(",\n        \"creator_trailer\": ")
        || !writer.append_uint(character.creatorTrailer)
        || !writer.append(",\n        \"equipment\": ")
        || !serialize_equipment(writer, character.equipment, 8U)
        || !writer.append(",\n        \"inventory\": ")
        || !serialize_inventory(writer, character.inventory, 8U)
        || !writer.append("\n      }") || !writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}

[[nodiscard]] inline bool rewrite_characters(std::string_view document,
                                             std::string_view serializedCharacter,
                                             std::span<char> storage,
                                             std::size_t& written) noexcept {
    written = 0;
    const std::size_t rootBegin = skip_whitespace(document, 0);
    std::size_t rootEnd = 0;
    if (rootBegin >= document.size() || document[rootBegin] != '{'
        || !scan_value(document, rootBegin, rootEnd)) {
        return false;
    }

    MemberSpan stateMember{};
    if (!find_object_member(document, rootBegin, rootEnd, "state", stateMember)
        || !stateMember.found || document[stateMember.valueBegin] != '{') {
        return false;
    }
    MemberSpan charactersMember{};
    if (!find_object_member(document,
                            stateMember.valueBegin,
                            stateMember.valueEnd,
                            "characters",
                            charactersMember)
        || !charactersMember.found || charactersMember.valueBegin >= charactersMember.valueEnd
        || document[charactersMember.valueBegin] != '['
        || document[charactersMember.valueEnd - 1U] != ']') {
        return false;
    }

    const std::size_t close = charactersMember.valueEnd - 1U;
    std::size_t insert = close;
    while (insert > charactersMember.valueBegin + 1U && whitespace(document[insert - 1U])) {
        --insert;
    }
    const std::size_t firstContent = skip_whitespace(document, charactersMember.valueBegin + 1U);
    const bool nonEmpty = firstContent < close && document[firstContent] != ']';

    Writer writer(storage);
    if (!writer.append(document.substr(0, insert))) {
        return false;
    }
    if (nonEmpty) {
        if (!writer.append(",\n") || !writer.append(serializedCharacter)) {
            return false;
        }
    } else if (!writer.append("\n") || !writer.append(serializedCharacter)
               || !writer.append("\n    ")) {
        return false;
    }
    if (!writer.append(document.substr(insert)) || !writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}

/** Removes one dense state.characters element while preserving every surviving object verbatim. */
[[nodiscard]] inline bool rewrite_character_removal(std::string_view document,
                                                     std::size_t removeIndex,
                                                     std::span<char> storage,
                                                     std::size_t& written) noexcept {
    written = 0;
    const std::size_t rootBegin = skip_whitespace(document, 0);
    std::size_t rootEnd = 0;
    if (rootBegin >= document.size() || document[rootBegin] != '{'
        || !scan_value(document, rootBegin, rootEnd)) {
        return false;
    }

    MemberSpan stateMember{};
    if (!find_object_member(document, rootBegin, rootEnd, "state", stateMember)
        || !stateMember.found || document[stateMember.valueBegin] != '{') {
        return false;
    }
    MemberSpan charactersMember{};
    if (!find_object_member(document,
                            stateMember.valueBegin,
                            stateMember.valueEnd,
                            "characters",
                            charactersMember)
        || !charactersMember.found || charactersMember.valueBegin >= charactersMember.valueEnd
        || document[charactersMember.valueBegin] != '['
        || document[charactersMember.valueEnd - 1U] != ']') {
        return false;
    }

    struct ElementSpan {
        std::size_t begin{};
        std::size_t end{};
    };
    std::array<ElementSpan, state::kCharacterCapacity> elements{};
    std::size_t elementCount = 0;
    const std::size_t close = charactersMember.valueEnd - 1U;
    std::size_t position = skip_whitespace(document, charactersMember.valueBegin + 1U);
    while (position < close) {
        if (elementCount >= elements.size()) {
            return false;
        }
        const std::size_t begin = position;
        std::size_t end = 0;
        if (!scan_value(document, begin, end) || end > close) {
            return false;
        }
        elements[elementCount++] = ElementSpan{begin, end};
        position = skip_whitespace(document, end);
        if (position == close) {
            break;
        }
        if (position > close || document[position] != ',') {
            return false;
        }
        position = skip_whitespace(document, position + 1U);
    }
    if (removeIndex >= elementCount) {
        return false;
    }

    Writer writer(storage);
    if (!writer.append(document.substr(0, charactersMember.valueBegin + 1U))) {
        return false;
    }
    bool wrote = false;
    for (std::size_t index = 0; index < elementCount; ++index) {
        if (index == removeIndex) {
            continue;
        }
        if (!writer.append(wrote ? ",\n      " : "\n      ")
            || !writer.append(document.substr(elements[index].begin,
                                              elements[index].end - elements[index].begin))) {
            return false;
        }
        wrote = true;
    }
    if (!writer.append("\n    ") || !writer.append(document.substr(close)) || !writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}


/** Compares two characters only through the fields persisted in settings.json. */
[[nodiscard]] inline bool same_persisted_character(const state::CharacterState& left,
                                                   const state::CharacterState& right) noexcept {
    std::array<char, 32768> leftStorage{};
    std::array<char, 32768> rightStorage{};
    std::size_t leftSize = 0;
    std::size_t rightSize = 0;
    return serialize_character(left, leftStorage, leftSize)
           && serialize_character(right, rightStorage, rightSize) && leftSize == rightSize
           && std::string_view(leftStorage.data(), leftSize)
                  == std::string_view(rightStorage.data(), rightSize);
}

/** Compares the dense authored character roster, ignoring runtime-only selection state. */
[[nodiscard]] inline bool same_persisted_roster(const state::AccountState& left,
                                                const state::AccountState& right) noexcept {
    if (left.characterCount != right.characterCount
        || left.characterCount > left.characters.size()
        || right.characterCount > right.characters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.characterCount; ++index) {
        if (!same_persisted_character(left.characters[index], right.characters[index])) {
            return false;
        }
    }
    return true;
}

/** Replaces state.characters with one canonical runtime roster. */
[[nodiscard]] inline bool rewrite_character_roster(std::string_view document,
                                                   const state::AccountState& account,
                                                   std::span<char> storage,
                                                   std::size_t& written) noexcept {
    written = 0;
    if (account.characterCount > account.characters.size()) {
        return false;
    }

    const std::size_t rootBegin = skip_whitespace(document, 0);
    std::size_t rootEnd = 0;
    if (rootBegin >= document.size() || document[rootBegin] != '{'
        || !scan_value(document, rootBegin, rootEnd)) {
        return false;
    }

    MemberSpan stateMember{};
    if (!find_object_member(document, rootBegin, rootEnd, "state", stateMember)
        || !stateMember.found || document[stateMember.valueBegin] != '{') {
        return false;
    }
    MemberSpan charactersMember{};
    if (!find_object_member(document,
                            stateMember.valueBegin,
                            stateMember.valueEnd,
                            "characters",
                            charactersMember)
        || !charactersMember.found || charactersMember.valueBegin >= charactersMember.valueEnd
        || document[charactersMember.valueBegin] != '['
        || document[charactersMember.valueEnd - 1U] != ']') {
        return false;
    }

    Writer writer(storage);
    if (!writer.append(document.substr(0, charactersMember.valueBegin))
        || !writer.append("[")) {
        return false;
    }
    std::array<char, 32768> serializedBuffer{};
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        std::size_t serializedSize = 0;
        if (!serialize_character(account.characters[index], serializedBuffer, serializedSize)
            || !writer.append(index == 0 ? "\n" : ",\n")
            || !writer.append({serializedBuffer.data(), serializedSize})) {
            return false;
        }
    }
    if (account.characterCount != 0 && !writer.append("\n    ")) {
        return false;
    }
    if (!writer.append("]") || !writer.append(document.substr(charactersMember.valueEnd))
        || !writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}

/** Validates that a rewritten settings document persists exactly one canonical runtime roster. */
[[nodiscard]] inline bool validate_character_roster(std::string_view document,
                                                    const state::AccountState& expected,
                                                    Settings& parsed) noexcept {
    return parse(document, parsed)
           && same_persisted_roster(parsed.initialAccount, expected);
}

[[nodiscard]] inline bool read_settings(const path::Buffer& filePath,
                                        std::span<char> storage,
                                        std::size_t& readSize) noexcept {
    readSize = 0;
    const HANDLE file = CreateFileW(filePath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) > storage.size()) {
        CloseHandle(file);
        return false;
    }
    const DWORD requested = static_cast<DWORD>(size.QuadPart);
    DWORD bytesRead = 0;
    const bool complete =
        ReadFile(file, storage.data(), requested, &bytesRead, nullptr) != FALSE
        && bytesRead == requested;
    const bool closed = CloseHandle(file) != FALSE;
    if (!complete || !closed) {
        return false;
    }
    readSize = static_cast<std::size_t>(bytesRead);
    return true;
}

[[nodiscard]] inline bool store_settings(const path::Buffer& filePath,
                                         std::string_view document) noexcept {
    path::Buffer stagePath = filePath;
    if (!path::append(stagePath, kStageFileSuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(stagePath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytesWritten = 0;
    const bool sizeFits = document.size() <= (std::numeric_limits<DWORD>::max)();
    const DWORD requested = sizeFits ? static_cast<DWORD>(document.size()) : 0;
    const bool wrote = sizeFits
                       && WriteFile(file, document.data(), requested, &bytesWritten, nullptr) != FALSE
                       && bytesWritten == requested;
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    const bool moved = flushed && closed
                       && MoveFileExW(stagePath.chars.data(),
                                      filePath.chars.data(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                              != FALSE;
    if (!moved) {
        (void)DeleteFileW(stagePath.chars.data());
    }
    return moved;
}

inline void report_store(std::string_view result,
                         std::string_view reason,
                         std::uint64_t soid,
                         std::size_t count) noexcept {
    std::array<char, log::kLineCapacity> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=character_persistence stage=store result=%.*s reason=%.*s "
                                     "mode=characters soid=0x%016llX count=%zu",
                                     static_cast<int>(result.size()),
                                     result.data(),
                                     static_cast<int>(reason.size()),
                                     reason.data(),
                                     static_cast<unsigned long long>(soid),
                                     count);
    if (length > 0) {
        log::write(log::Channel::state,
                   result == "ok" ? log::Level::info : log::Level::warn,
                   {line.data(), static_cast<std::size_t>(length)});
    }
}

inline void report_remove(std::string_view result,
                          std::string_view reason,
                          std::uint64_t soid,
                          std::size_t count) noexcept {
    std::array<char, log::kLineCapacity> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=character_persistence stage=remove result=%.*s reason=%.*s "
                                     "mode=characters soid=0x%016llX count=%zu",
                                     static_cast<int>(result.size()),
                                     result.data(),
                                     static_cast<int>(reason.size()),
                                     reason.data(),
                                     static_cast<unsigned long long>(soid),
                                     count);
    if (length > 0) {
        log::write(log::Channel::state,
                   result == "ok" ? log::Level::info : log::Level::warn,
                   {line.data(), static_cast<std::size_t>(length)});
    }
}

} // namespace detail

/**
 * Persists one opcode-501 after-image against the exact runtime roster that produced it.
 *
 * Normal operation appends only the new authored row and preserves every existing row verbatim.
 * If settings.json drifted from runtime State, the whole characters array is rebuilt from the
 * canonical runtime after-image instead of rejecting a stale duplicate SOID.
 */
[[nodiscard]] inline bool store_character(const state::AccountState& before,
                                          const state::AccountState& after,
                                          std::uint64_t characterSoid) noexcept {
    AcquireSRWLockExclusive(&detail::g_storeLock);

    bool stored = false;
    std::string_view reason = "unknown";
    std::size_t finalCount = 0;
    do {
        if (before.primarySoid == 0 || after.primarySoid != before.primarySoid
            || before.characterCount >= before.characters.size()
            || after.characterCount != before.characterCount + 1U
            || after.characterCount > after.characters.size()
            || before.characterCount >= after.characterCount
            || after.characters[before.characterCount].soid != characterSoid
            || characterSoid == 0) {
            reason = "identity";
            break;
        }

        path::Buffer filePath{};
        if (!detail::settings_path(filePath)) {
            reason = "path";
            break;
        }

        static std::array<char, detail::kSettingsCapacity> readBuffer{};
        std::size_t readSize = 0;
        if (!detail::read_settings(filePath, readBuffer, readSize)) {
            reason = "read";
            break;
        }
        std::string_view document(readBuffer.data(), readSize);
        if (document.size() >= 3 && static_cast<unsigned char>(document[0]) == 0xEFU
            && static_cast<unsigned char>(document[1]) == 0xBBU
            && static_cast<unsigned char>(document[2]) == 0xBFU) {
            document.remove_prefix(3);
        }

        static Settings current{};
        if (!parse(document, current)) {
            reason = "parse";
            break;
        }

        // A prior disk write may already have landed even if runtime State did not advance.
        if (detail::same_persisted_roster(current.initialAccount, after)) {
            finalCount = after.characterCount;
            reason = "already";
            stored = true;
            break;
        }

        static std::array<char, detail::kRewriteCapacity> rewriteBuffer{};
        std::size_t rewriteSize = 0;
        if (detail::same_persisted_roster(current.initialAccount, before)) {
            std::array<char, 32768> serializedBuffer{};
            std::size_t serializedSize = 0;
            if (!detail::serialize_character(
                    after.characters[before.characterCount], serializedBuffer, serializedSize)) {
                reason = "serialize";
                break;
            }
            if (!detail::rewrite_characters(document,
                                            {serializedBuffer.data(), serializedSize},
                                            rewriteBuffer,
                                            rewriteSize)) {
                reason = "rewrite";
                break;
            }
            reason = "ok";
        } else {
            // Disk and State disagree. State is authoritative for this transaction, so repair the
            // authored roster in the same atomic write that persists the new character.
            if (!detail::rewrite_character_roster(
                    document, after, rewriteBuffer, rewriteSize)) {
                reason = "rewrite";
                break;
            }
            reason = "reconcile";
        }

        const std::string_view rewritten(rewriteBuffer.data(), rewriteSize);
        static Settings validation{};
        if (!detail::validate_character_roster(rewritten, after, validation)) {
            reason = "validate";
            break;
        }
        finalCount = validation.initialAccount.characterCount;
        if (!detail::store_settings(filePath, rewritten)) {
            reason = "write";
            break;
        }
        stored = true;
    } while (false);

    ReleaseSRWLockExclusive(&detail::g_storeLock);
    detail::report_store(stored ? "ok" : "fail", reason, characterSoid, finalCount);
    return stored;
}

/**
 * Persists one opcode-502 after-image against the exact runtime roster that produced it.
 *
 * Normal operation removes the dense authored row while preserving survivors verbatim. A drifted
 * settings.json is repaired from the runtime after-image rather than deleting an unrelated row.
 */
[[nodiscard]] inline bool remove_character(const state::AccountState& before,
                                           const state::AccountState& after,
                                           std::uint64_t characterSoid,
                                           std::size_t expectedIndex) noexcept {
    AcquireSRWLockExclusive(&detail::g_storeLock);

    bool removed = false;
    std::string_view reason = "unknown";
    std::size_t finalCount = 0;
    do {
        if (before.primarySoid == 0 || after.primarySoid != before.primarySoid
            || characterSoid == 0 || before.characterCount == 0
            || before.characterCount != after.characterCount + 1U
            || expectedIndex >= before.characterCount
            || before.characters[expectedIndex].soid != characterSoid) {
            reason = "identity";
            break;
        }

        path::Buffer filePath{};
        if (!detail::settings_path(filePath)) {
            reason = "path";
            break;
        }

        static std::array<char, detail::kSettingsCapacity> readBuffer{};
        std::size_t readSize = 0;
        if (!detail::read_settings(filePath, readBuffer, readSize)) {
            reason = "read";
            break;
        }
        std::string_view document(readBuffer.data(), readSize);
        if (document.size() >= 3 && static_cast<unsigned char>(document[0]) == 0xEFU
            && static_cast<unsigned char>(document[1]) == 0xBBU
            && static_cast<unsigned char>(document[2]) == 0xBFU) {
            document.remove_prefix(3);
        }

        static Settings current{};
        if (!parse(document, current)) {
            reason = "parse";
            break;
        }

        // A retry after the atomic disk write is already on the desired side of the deletion.
        if (detail::same_persisted_roster(current.initialAccount, after)) {
            finalCount = after.characterCount;
            reason = "already";
            removed = true;
            break;
        }

        static std::array<char, detail::kRewriteCapacity> rewriteBuffer{};
        std::size_t rewriteSize = 0;
        if (detail::same_persisted_roster(current.initialAccount, before)) {
            if (!detail::rewrite_character_removal(
                    document, expectedIndex, rewriteBuffer, rewriteSize)) {
                reason = "rewrite";
                break;
            }
            reason = "ok";
        } else {
            // Never choose a row by stale disk SOID/index. Repair to the exact runtime after-image.
            if (!detail::rewrite_character_roster(
                    document, after, rewriteBuffer, rewriteSize)) {
                reason = "rewrite";
                break;
            }
            reason = "reconcile";
        }

        const std::string_view rewritten(rewriteBuffer.data(), rewriteSize);
        static Settings validation{};
        if (!detail::validate_character_roster(rewritten, after, validation)) {
            reason = "validate";
            break;
        }
        finalCount = validation.initialAccount.characterCount;
        if (!detail::store_settings(filePath, rewritten)) {
            reason = "write";
            break;
        }
        removed = true;
    } while (false);

    ReleaseSRWLockExclusive(&detail::g_storeLock);
    detail::report_remove(removed ? "ok" : "fail", reason, characterSoid, finalCount);
    return removed;
}

} // namespace sunrise::core::settings::persistence
