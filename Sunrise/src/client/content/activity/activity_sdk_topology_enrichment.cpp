#include "activity_sdk_topology_enrichment.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../state/build_data/scriptables/definition.h"

namespace sunrise::client::content::activity::sdk_generation::topology_enrichment {
namespace {

namespace format = state::activity_sdk::format;
namespace catalog = state::build_data::scriptables;

/** Rejects malformed, overlong, surrogate, and embedded-NUL UTF-8. */
[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first == 0) {
            return false;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first <= 0x7FU) {
            count = 1;
            codepoint = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            count = 2;
            codepoint = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            count = 3;
            codepoint = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > value.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        index += count;
    }
    return true;
}

[[nodiscard]] bool byte_less(std::string_view left, std::string_view right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) noexcept {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
        });
}

[[nodiscard]] std::uint32_t content_hash(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : value) {
        hash = (hash * 16777619U) ^ byte;
    }
    return hash;
}

/** Returns one bounded inline name only when its bytes form valid UTF-8. */
[[nodiscard]] bool inline_view(const topology_inventory::Snapshot& topology,
                               std::uint32_t row,
                               std::string_view& output) noexcept {
    output = {};
    if (row >= topology.inlineNames.size()) {
        return false;
    }
    const topology_inventory::InlineName& name = topology.inlineNames[row];
    if (name.firstByte > topology.inlineNameBytes.size()
        || name.byteCount > topology.inlineNameBytes.size() - name.firstByte
        || name.byteCount >= topology_inventory::Text{}.value.size()) {
        return false;
    }
    output = std::string_view(
        reinterpret_cast<const char*>(topology.inlineNameBytes.data() + name.firstByte),
        name.byteCount);
    return valid_utf8(output);
}

/** Requires packed byte coverage and strict hash-plus-byte ordering for inline names. */
[[nodiscard]] bool valid_inline_bank(const topology_inventory::Snapshot& topology) noexcept {
    std::size_t nextByte = 0;
    std::uint32_t priorHash = 0;
    std::string_view priorValue{};
    for (std::size_t index = 0; index < topology.inlineNames.size(); ++index) {
        const topology_inventory::InlineName& name = topology.inlineNames[index];
        std::string_view value{};
        if (name.firstByte != nextByte || name.byteCount == 0
            || name.byteCount > catalog::kInlineNameMaximumBytes
            || !inline_view(topology, static_cast<std::uint32_t>(index), value)
            || content_hash(value) != name.hash
            || (index != 0
                && (priorHash > name.hash
                    || (priorHash == name.hash && !byte_less(priorValue, value))))) {
            return false;
        }
        priorHash = name.hash;
        priorValue = value;
        nextByte += name.byteCount;
    }
    return nextByte == topology.inlineNameBytes.size();
}

/** Copies one validated inline name into fixed pack text storage. */
[[nodiscard]] bool copy_inline(const topology_inventory::Snapshot& topology,
                               std::uint32_t row,
                               topology_inventory::Text& output) noexcept {
    output = {};
    std::string_view value{};
    if (!inline_view(topology, row, value)) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.value.begin());
    output.length = static_cast<std::uint16_t>(value.size());
    return true;
}

[[nodiscard]] bool same_text(const topology_inventory::Text& left,
                             const topology_inventory::Text& right) noexcept {
    return left.length == right.length && left.length < left.value.size()
           && right.length < right.value.size()
           && std::equal(left.value.begin(), left.value.begin() + left.length, right.value.begin());
}

/** Validates one observed alias range and returns its selected name. */
[[nodiscard]] bool resolve_name(const topology_inventory::Snapshot& topology,
                                std::uint32_t nameHash,
                                std::uint32_t observedNameRow,
                                std::uint32_t firstAlias,
                                std::uint32_t aliasCount,
                                topology_inventory::Text& selected,
                                std::vector<topology_inventory::Text>* aliases) {
    selected = {};
    if (firstAlias > topology.observedAliases.size()
        || aliasCount > topology.observedAliases.size() - firstAlias
        || (aliasCount == 1) != (observedNameRow != catalog::kNoRow)) {
        return false;
    }
    std::string_view previous{};
    for (std::uint32_t ordinal = 0; ordinal < aliasCount; ++ordinal) {
        const std::uint32_t aliasRow = topology.observedAliases[firstAlias + ordinal];
        if (aliasRow >= topology.inlineNames.size()
            || topology.inlineNames[aliasRow].hash != nameHash) {
            return false;
        }
        std::string_view value{};
        topology_inventory::Text owned{};
        if (!inline_view(topology, aliasRow, value) || (ordinal != 0 && !byte_less(previous, value))
            || !copy_inline(topology, aliasRow, owned)) {
            return false;
        }
        previous = value;
        if (aliases != nullptr) {
            aliases->push_back(owned);
        }
    }
    if (aliasCount == 1) {
        if (observedNameRow != topology.observedAliases[firstAlias]
            || !copy_inline(topology, observedNameRow, selected)) {
            return false;
        }
    }
    return true;
}

/** Emits one stable schema identity directly from its package handle. */
[[nodiscard]] bool make_schema_id(std::uint32_t handle, topology_inventory::Text& output) noexcept {
    output = {};
    if (handle == 0 || handle == format::kAbsentIndex) {
        return true;
    }
    const int length = std::snprintf(
        output.value.data(), output.value.size(), "schema/%08x", static_cast<unsigned>(handle));
    if (length < 0 || static_cast<std::size_t>(length) >= output.value.size()
        || static_cast<unsigned>(length) > (std::numeric_limits<std::uint16_t>::max)()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(length);
    return true;
}

[[nodiscard]] std::uint32_t unique_or_absent(std::vector<std::uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values.size() == 1 ? values.front() : format::kAbsentIndex;
}

/** Rebuilds one slot's aliases and schema joins from descriptor evidence. */
[[nodiscard]] bool resolve_slot(const topology_inventory::Snapshot& topology,
                                const topology_inventory::Slot& source,
                                std::vector<topology_inventory::Text>& aliases,
                                Slot& output) {
    output = {};
    if (source.componentClass != format::kAbsentIndex || source.senseSchema != format::kAbsentIndex
        || source.authSchema != format::kAbsentIndex || source.flags != 0
        || source.descriptorCount == format::kAbsentIndex
        || source.descriptorEvidence.size() != source.descriptorCount
        || aliases.size() >= format::kAbsentIndex) {
        return false;
    }
    output.aliases.first = static_cast<std::uint32_t>(aliases.size());
    if (!resolve_name(topology,
                      source.nameHash,
                      source.observedNameRow,
                      source.firstObservedAlias,
                      source.observedAliasCount,
                      output.name,
                      &aliases)
        || aliases.size() - output.aliases.first >= format::kAbsentIndex) {
        return false;
    }
    output.aliases.count = static_cast<std::uint32_t>(aliases.size() - output.aliases.first);

    std::vector<std::uint32_t> components{};
    std::vector<std::uint32_t> sense{};
    std::vector<std::uint32_t> auth{};
    components.reserve(source.descriptorEvidence.size());
    sense.reserve(source.descriptorEvidence.size());
    auth.reserve(source.descriptorEvidence.size());
    for (const topology_inventory::DescriptorEvidence& descriptor : source.descriptorEvidence) {
        components.push_back(descriptor.componentClass);
        if (descriptor.senseSchema != 0 && descriptor.senseSchema != format::kAbsentIndex) {
            sense.push_back(descriptor.senseSchema);
        }
        if (descriptor.authSchema != 0 && descriptor.authSchema != format::kAbsentIndex) {
            auth.push_back(descriptor.authSchema);
        }
    }
    output.componentClass = unique_or_absent(components);
    output.senseSchema = unique_or_absent(sense);
    output.authSchema = unique_or_absent(auth);
    if (source.descriptorComponentClass != output.componentClass
        || source.descriptorSenseSchema != output.senseSchema
        || source.descriptorAuthSchema != output.authSchema) {
        return false;
    }
    if (source.descriptorCount != 0 && components.size() <= 1 && sense.size() <= 1
        && auth.size() <= 1) {
        output.flags = format::kSlotSchemaJoinExact;
    }
    return make_schema_id(output.senseSchema, output.senseSchemaId)
           && make_schema_id(output.authSchema, output.authSchemaId);
}

[[nodiscard]] bool same_slot(const Slot& left, const Slot& right) noexcept {
    return same_text(left.name, right.name) && same_text(left.senseSchemaId, right.senseSchemaId)
           && same_text(left.authSchemaId, right.authSchemaId)
           && left.componentClass == right.componentClass && left.senseSchema == right.senseSchema
           && left.authSchema == right.authSchema && left.flags == right.flags
           && left.dialogueCueCount == right.dialogueCueCount
           && left.outputChannelCount == right.outputChannelCount
           && left.aliases.first == right.aliases.first
           && left.aliases.count == right.aliases.count;
}

} // namespace

/** Recomputes every name and schema join and rejects output drift. */
bool validate(const topology_inventory::Snapshot& topology, const Snapshot& enrichment) noexcept {
    if (!topology.ready || topology.nameInventoryComplete || topology.stringInventoryComplete
        || !valid_inline_bank(topology) || enrichment.bubbleNames.size() != topology.bubbles.size()
        || enrichment.slots.size() != topology.slots.size()
        || enrichment.slotAliases.size() >= format::kAbsentIndex) {
        return false;
    }
    std::size_t aliasCursor = 0;
    for (std::size_t index = 0; index < topology.bubbles.size(); ++index) {
        topology_inventory::Text expected{};
        if (!resolve_name(topology,
                          topology.bubbles[index].nameHash,
                          topology.bubbles[index].observedNameRow,
                          topology.bubbles[index].firstObservedAlias,
                          topology.bubbles[index].observedAliasCount,
                          expected,
                          nullptr)
            || !same_text(expected, enrichment.bubbleNames[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < topology.slots.size(); ++index) {
        std::vector<topology_inventory::Text> aliases{};
        Slot expected{};
        if (!resolve_slot(topology, topology.slots[index], aliases, expected)
            || expected.aliases.first != 0 || aliasCursor > enrichment.slotAliases.size()
            || aliases.size() > enrichment.slotAliases.size() - aliasCursor) {
            return false;
        }
        expected.aliases.first = static_cast<std::uint32_t>(aliasCursor);
        if (!same_slot(expected, enrichment.slots[index])) {
            return false;
        }
        for (std::size_t ordinal = 0; ordinal < aliases.size(); ++ordinal) {
            if (!same_text(aliases[ordinal], enrichment.slotAliases[aliasCursor + ordinal])) {
                return false;
            }
        }
        aliasCursor += aliases.size();
    }
    return aliasCursor == enrichment.slotAliases.size();
}

/** Resolves exact inline names and package descriptor candidates transactionally. */
bool build_impl(const topology_inventory::Snapshot& topology,
                bool validateOutput,
                Snapshot& output) noexcept {
    output = {};
    if (!topology.ready || topology.nameInventoryComplete || topology.stringInventoryComplete
        || !valid_inline_bank(topology)) {
        return false;
    }
    try {
        Snapshot pending{};
        pending.bubbleNames.reserve(topology.bubbles.size());
        pending.slots.reserve(topology.slots.size());
        pending.slotAliases.reserve(topology.observedAliases.size());
        for (const topology_inventory::Bubble& bubble : topology.bubbles) {
            topology_inventory::Text selected{};
            if (!resolve_name(topology,
                              bubble.nameHash,
                              bubble.observedNameRow,
                              bubble.firstObservedAlias,
                              bubble.observedAliasCount,
                              selected,
                              nullptr)) {
                return false;
            }
            pending.bubbleNames.push_back(selected);
        }
        for (const topology_inventory::Slot& source : topology.slots) {
            Slot slot{};
            if (!resolve_slot(topology, source, pending.slotAliases, slot)) {
                return false;
            }
            pending.slots.push_back(slot);
        }
        if (validateOutput && !validate(topology, pending)) {
            return false;
        }
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool build(const topology_inventory::Snapshot& topology, Snapshot& output) noexcept {
    return build_impl(topology, true, output);
}

bool build_generated(const topology_inventory::Snapshot& topology, Snapshot& output) noexcept {
    return build_impl(topology, false, output);
}

} // namespace sunrise::client::content::activity::sdk_generation::topology_enrichment
