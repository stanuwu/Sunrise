#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "activity_sdk_actor_rsat_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {

namespace format = state::activity_sdk::format;
namespace reader = middleware::content::packages::reader;

/** Zero and the all-ones sentinel do not name a package row. */
inline constexpr std::uint32_t kAbsentTag = 0xFFFFFFFFU;

/** One validated typed-array descriptor and its exact stored metadata. */
struct TypedArray final {
    std::uint32_t count{};
    std::int64_t relative{};
    std::uint32_t headerOffset{format::kAbsentIndex};
    std::uint32_t dataOffset{format::kAbsentIndex};
    std::uint32_t elementClass{format::kAbsentIndex};
    bool typed{};
};

/** One schema and its raw fields before global schema ordering is known. */
struct SchemaSource final {
    RsatSchema row{};
    std::vector<RsatField> fields{};
};

/** One decoded ActorGroup source, or an unrelated component remembered as absent. */
struct SequenceOwner final {
    std::uint32_t sourceOffset{};
    std::uint32_t globalTag{};
    std::uint32_t localTag{};
    bool present{};
};

/** State owned by one transactional build. */
struct BuildState final {
    ReadTag readTag{};
    void* readContext{};
    CancelProbe cancel{};
    void* cancelContext{};
    Snapshot snapshot{};
    std::vector<SchemaSource> schemaSources{};
    std::unordered_map<std::uint32_t, std::size_t> schemaIndexes{};
    std::unordered_set<std::uint32_t> rsatTags{};
    std::unordered_map<std::uint32_t, std::uint32_t> sequenceTableIndexes{};
    std::unordered_map<std::uint32_t, SequenceOwner> sequenceOwners{};
};

/** Actual reader callback context. */
struct PrefetchedTag final {
    std::vector<std::byte> bytes{};
    std::uint32_t classId{};
};

/** Package rows already held in memory, plus the reader used for every other tag. */
struct PackageReadContext final {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    std::unordered_map<std::uint32_t, PrefetchedTag> prefetched{};
};

/** @return True when the caller asked the build to stop. */
[[nodiscard]] inline bool is_cancelled(CancelProbe probe, void* context) noexcept {
    return probe != nullptr && probe(context);
}

/** @return True when the tag names no package row. */
[[nodiscard]] inline bool is_absent_tag(std::uint32_t tag) noexcept {
    return tag == 0 || tag == kAbsentTag;
}

/** @return True when one complete range lies in the package blob. */
[[nodiscard]] inline bool
contains(std::span<const std::byte> blob, std::size_t offset, std::size_t size) noexcept {
    return offset <= blob.size() && size <= blob.size() - offset;
}

/**
 * Copies one fixed-size value out of a package blob.
 * @param blob Package row bytes.
 * @param offset Byte offset of the value.
 * @param output Receives the value; zeroed when the range does not fit.
 * @return False when the range does not fit the blob.
 */
template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    output = {};
    if (!contains(blob, offset, sizeof output)) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

/** Narrows one host size to the 32-bit width every stored offset uses. */
[[nodiscard]] inline bool to_u32(std::size_t value, std::uint32_t& output) noexcept {
    output = 0;
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Adds one signed self-relative field without wrapping host arithmetic. */
[[nodiscard]] inline bool
relative_offset(std::size_t base, std::int64_t relative, std::size_t& output) noexcept {
    // The blob stores self-relative offsets as int64, so its bounds are the limit.
    constexpr std::int64_t kMaximum = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t kMinimum = (std::numeric_limits<std::int64_t>::min)();
    output = 0;
    if (base > static_cast<std::size_t>(kMaximum)) {
        return false;
    }
    const auto signedBase = static_cast<std::int64_t>(base);
    if ((relative > 0 && signedBase > kMaximum - relative)
        || (relative < 0 && signedBase < kMinimum - relative)) {
        return false;
    }
    const std::int64_t target = signedBase + relative;
    if (target < 0) {
        return false;
    }
    output = static_cast<std::size_t>(target);
    return true;
}

/** Formats the stable actor-class ID used by the generated pack. */
[[nodiscard]] inline bool format_actor_id(std::uint32_t tag, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(
        output.value.data(), output.value.size(), "actor-class/%08x", static_cast<unsigned>(tag));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Formats the stable RSAT schema ID used by the generated pack. */
[[nodiscard]] inline bool format_schema_id(std::uint32_t tag, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(
        output.value.data(), output.value.size(), "rsat-schema/%08x", static_cast<unsigned>(tag));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Formats the stable tag-and-ordinal ID for one RSAT descriptor. */
[[nodiscard]] inline bool
format_descriptor_id(std::uint32_t tag, std::uint32_t ordinal, Text& output) noexcept {
    output = {};
    const int written = std::snprintf(output.value.data(),
                                      output.value.size(),
                                      "rsat-descriptor/%08x/%06x",
                                      static_cast<unsigned>(tag),
                                      static_cast<unsigned>(ordinal));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** Reads the exact `{i64 count, i64 self-relative header}` package array. */
[[nodiscard]] bool typed_array(std::span<const std::byte> blob,
                               std::size_t field,
                               std::uint32_t expectedClass,
                               std::size_t stride,
                               TypedArray& output) noexcept;

/** Appends the state names one actor class can be told to enter, in authored order. */
[[nodiscard]] bool collect_state_names(BuildState& state,
                                       std::uint32_t actorIndex,
                                       std::span<const std::byte> actorBlob);

/** Reads and retains one schema exactly once by tag. */
[[nodiscard]] bool schema(BuildState& state, std::uint32_t tag, std::size_t& sourceIndex) noexcept;

/** Reads one installed SObject RSAT and retains its complete ordered component layout. */
[[nodiscard]] bool sobject_rsat(BuildState& state, std::uint32_t tag) noexcept;

/** Reads one package row and confirms its physical class. */
[[nodiscard]] bool package_read(void* opaque,
                                std::uint32_t tag,
                                std::uint32_t expectedClass,
                                std::vector<std::byte>& output) noexcept;

/** Adds the pinned actor command semantics shared by every installed actor. */
[[nodiscard]] bool add_engine_semantics(Snapshot& snapshot);

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
