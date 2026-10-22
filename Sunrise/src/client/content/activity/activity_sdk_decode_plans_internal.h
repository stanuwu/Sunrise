#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/crypto/sha256.h"
#include "activity_sdk_decode_plans.h"
#include "activity_sdk_decode_plans_image.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {

namespace reader = middleware::content::packages::reader;
using Digest = middleware::crypto::sha256::Digest;

/** Absent handle or index in package and reflection data. */
inline constexpr std::uint32_t kAbsent = 0xFFFFFFFFU;
/** Reflection record block chain marker and the generic-argument marker before it. */
inline constexpr std::uint32_t kReflectionMarker = 0x80800050U;
inline constexpr std::uint32_t kGenericMarker = 0x8080001DU;
/** Reflection block classes: field codec, property list, method list, empty tail. */
inline constexpr std::uint32_t kCodecBlock = 0x8080010AU;
inline constexpr std::uint32_t kPropertyBlock = 0x8080012FU;
inline constexpr std::uint32_t kMethodBlock = 0x8080010DU;
inline constexpr std::uint32_t kTailBlock = 0x8080004CU;
/** Generic container kinds a property may declare. */
inline constexpr std::uint32_t kGenericSimple = 0x80809C37U;
inline constexpr std::uint32_t kGenericDynamicA = 0x80809FBAU;
inline constexpr std::uint32_t kGenericDynamicB = 0x80809C38U;
inline constexpr std::uint32_t kDynamicDeclared = 0x80809FBBU;
inline constexpr std::uint32_t kGenericInline = 0x80809FFCU;
inline constexpr std::uint32_t kGenericCounted = 0x80809ED8U;
/** Package classes: the array marker, a selection child row, metadata, and a selection. */
inline constexpr std::uint32_t kArrayMarker = 0x80809FBDU;
inline constexpr std::uint32_t kSelectionChildClass = 0x80808852U;
inline constexpr std::uint32_t kPackageMetadataClass = 0x80800000U;
inline constexpr std::uint32_t kSelectionClass = 0x80809BBBU;
/** Metadata kinds that select a plan branch: plain value and generic container. */
inline constexpr std::uint8_t kKindValue = 1;
inline constexpr std::uint8_t kKindContainer = 11;
/** Field type code of a nested schema; codes above 63 have no primitive size. */
inline constexpr std::uint8_t kTypeNested = 1;
inline constexpr std::uint8_t kTypeLimit = 64;
/** Capacities the runtime decoder shares with the compiler. */
inline constexpr std::size_t kMaximumDepth = 32;
inline constexpr std::size_t kMaximumSteps = 65536;
inline constexpr std::uint32_t kMaximumRepeat = 16384;
inline constexpr std::uint32_t kMaximumSectionCount = 65536;
inline constexpr std::uint64_t kMaximumArrayCount = 0x100000U;
/** Stride of one codec field descriptor. */
inline constexpr std::size_t kFieldRowSize = 40;
/** Candidate offsets of the block chain marker inside a reflection record. */
inline constexpr std::array<std::uint32_t, 4> kMarkerOffsets{0x58, 0x60, 0x68, 0x70};
/** Definition hash in the reflection record header. */
inline constexpr std::uint64_t kDefinitionHashAt = 0x08;
/** Bound handle in a codec block. */
inline constexpr std::uint64_t kCodecHandleAt = 0x10;

/** Header size and row stride of each reflection block class. */
struct BlockLayout final {
    std::uint32_t header{};
    std::uint32_t stride{};
};

/**
 * Looks up the header size and row stride of one reflection block class.
 * @return False for an unknown block class.
 */
[[nodiscard]] inline bool block_layout(std::uint32_t kind, BlockLayout& output) noexcept {
    switch (kind) {
    case kCodecBlock:
        output = {0x28, 0x28};
        return true;
    case kPropertyBlock:
        output = {0x20, 0x0C};
        return true;
    case kMethodBlock:
        output = {0x10, 0x08};
        return true;
    case kTailBlock:
        output = {0x18, 0x00};
        return true;
    default:
        return false;
    }
}

/** Copies one unaligned value. @return False when it does not fit inside `bytes`. */
template <typename T>
[[nodiscard]] bool read_at(std::span<const std::byte> bytes, std::size_t at, T& value) noexcept {
    if (at > bytes.size() || bytes.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return true;
}

/** One codec field descriptor in reflection or package layout. */
struct Field final {
    std::uint32_t offset{};
    std::uint32_t alternate{};
    std::uint32_t bit{};
    float half{};
    std::uint8_t type{};
    std::uint8_t presence{};
    std::uint16_t parameter2{};
    std::uint32_t nested{};
    std::int32_t bias{};
    std::int32_t width{};
    std::uint32_t parameter3{};
    std::uint32_t parameter4{};
};

/** One field codec block. */
struct Codec final {
    bool present{};
    std::uint32_t handle{};
    std::uint32_t original{};
    std::uint32_t serialized{};
    std::uint32_t flags{};
    std::uint32_t array{};
    std::vector<Field> fields{};
};

/** One declared property: name hash, member type handle, byte offset. */
struct Property final {
    std::uint32_t nameHash{};
    std::uint32_t memberType{};
    std::uint32_t byteOffset{};
};

/** One reflection or package metadata record. */
struct Metadata final {
    bool package{};
    std::uint32_t definitionHash{};
    std::uint32_t definitionClass{};
    std::uint32_t size{};
    std::uint8_t alignment{};
    std::uint8_t kind{};
    bool hasGeneric{};
    std::uint32_t generic{};
    Codec codec{};
    bool hasProperties{};
    Property base{};
    std::vector<Property> properties{};
};

/** One schema row of the cache: handle, serialized size, first field value, field count. */
struct SchemaRow final {
    std::uint32_t handle{};
    std::uint32_t size{};
    std::uint32_t first{};
    std::uint32_t count{};
};

/** Metadata records read so far, plus the package blob digests the evidence needs. */
class Store final {
public:
    Store(const Image& image, const reader::Source& source, reader::Scratch& scratch) noexcept
        : image_(image), source_(source), scratch_(scratch) {}

    /** @return The record, or null with the failure named in `error`. */
    [[nodiscard]] const Metadata* get(std::uint32_t handle, const char*& error) noexcept;
    [[nodiscard]] const Image& image() const noexcept {
        return image_;
    }
    [[nodiscard]] std::map<std::uint32_t, Digest>& package_hashes() noexcept {
        return packageHashes_;
    }
    [[nodiscard]] const std::map<std::uint32_t, Metadata>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] bool read_package(std::uint32_t tag,
                                    std::uint32_t expectedClass,
                                    std::vector<std::byte>& output) noexcept;

private:
    const Image& image_;
    const reader::Source& source_;
    reader::Scratch& scratch_;
    std::map<std::uint32_t, Metadata> entries_{};
    std::map<std::uint32_t, Digest> packageHashes_{};
};

/** One compiled plan entry in cache row order. */
struct PlanEntry final {
    std::uint32_t schemaHandle{};
    std::uint32_t repeatCount{};
    std::uint32_t guardBit{};
    std::uint32_t firstFieldBit{};
    std::uint32_t fieldBitStride{};
};

/** One compiled component and selection pair. */
struct Plan final {
    std::uint32_t component{};
    std::uint32_t schemaTag{};
    std::vector<PlanEntry> entries{};
    std::uint32_t bitmapBits{};
    bool active{};
};

/** Parses one bound reflection record at `address` for the compiler. */
[[nodiscard]] bool parse_native(const Image& image,
                                std::uint64_t address,
                                Metadata& output,
                                const char*& error) noexcept;

/** Parses one package metadata blob for the compiler. */
[[nodiscard]] bool parse_package(std::uint32_t handle,
                                 std::span<const std::byte> blob,
                                 Metadata& output,
                                 const char*& error) noexcept;

/** Resolves the presence bit guarding `offset` inside `handle`; -1 when no field covers it. */
[[nodiscard]] bool property_guard(Store& store,
                                  std::uint32_t handle,
                                  std::uint64_t offset,
                                  std::int64_t& guard,
                                  const char*& error) noexcept;

/** Compiles one selection blob against its component's reflection metadata. */
[[nodiscard]] bool compile(Store& store,
                           std::uint32_t component,
                           std::span<const std::byte> blob,
                           Plan& output,
                           const char*& error) noexcept;

/** Indexes every bound codec block into schema rows sorted by handle. */
[[nodiscard]] bool build_codec_index(const Image& image,
                                     std::vector<SchemaRow>& rows,
                                     std::vector<std::uint32_t>& fieldValues,
                                     const char*& error) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal
