#include <array>
#include <cstring>

#include "../../../middleware/content/packages/reader/layout.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808001.h"
#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal {
namespace {

namespace generated = middleware::content::packages::schema::generated;
using PackageMetadata = generated::PackageReflectionMetadata;
using CodecBlock = generated::ReflectionCodecBlock;
using PropertyBlock = generated::ReflectionPropertyBlock;

/** Reflection record header: length at 0, then struct size, alignment, kind, generic flag. */
constexpr std::uint64_t kSizeAt = offsetof(PackageMetadata, structSize);
constexpr std::uint64_t kAlignmentAt = offsetof(PackageMetadata, alignment);
constexpr std::uint64_t kKindAt = offsetof(PackageMetadata, kind);
constexpr std::uint64_t kGenericFlagAt = offsetof(PackageMetadata, genericFlag);
constexpr std::uint8_t kGenericFlag = 6;
/** Package metadata: the codec and property sections are self-relative offsets. */
constexpr std::size_t kPackageMinimumSize = sizeof(PackageMetadata);
constexpr std::size_t kPackageCodecAt = offsetof(PackageMetadata, rsatData);
constexpr std::size_t kPackagePropertiesAt = offsetof(PackageMetadata, propertySection);
constexpr std::size_t kPackageCodecHeader = sizeof(CodecBlock);
constexpr std::size_t kPackagePropertyHeader = sizeof(PropertyBlock);
using FieldRow = generated::C80800109;
using PropertyRow = generated::C8080012e;
/** Codec field row and property row; the executable and package records share both. */
constexpr std::size_t kPropertyRowSize = sizeof(PropertyRow);
static_assert(sizeof(FieldRow) == kFieldRowSize);

/** Reads one field descriptor from the executable. */
[[nodiscard]] bool read_field(const Image& image, std::uint64_t at, Field& field) noexcept {
    return image.u32(at + offsetof(FieldRow, fieldOffset), field.offset)
           && image.u32(at + offsetof(FieldRow, alternate), field.alternate)
           && image.u32(at + offsetof(FieldRow, bit), field.bit)
           && image.f32(at + offsetof(FieldRow, half), field.half)
           && image.u8(at + offsetof(FieldRow, fieldType), field.type)
           && image.u8(at + offsetof(FieldRow, presence), field.presence)
           && image.u16(at + offsetof(FieldRow, parameter2), field.parameter2)
           && image.u32(at + offsetof(FieldRow, nested), field.nested)
           && image.i32(at + offsetof(FieldRow, bias), field.bias)
           && image.i32(at + offsetof(FieldRow, width), field.width)
           && image.u32(at + offsetof(FieldRow, parameter3), field.parameter3)
           && image.u32(at + offsetof(FieldRow, parameter4), field.parameter4);
}

/** Reads one field descriptor from a package blob. */
[[nodiscard]] bool
read_field(std::span<const std::byte> blob, std::size_t at, Field& field) noexcept {
    return read_at(blob, at + offsetof(FieldRow, fieldOffset), field.offset)
           && read_at(blob, at + offsetof(FieldRow, alternate), field.alternate)
           && read_at(blob, at + offsetof(FieldRow, bit), field.bit)
           && read_at(blob, at + offsetof(FieldRow, half), field.half)
           && read_at(blob, at + offsetof(FieldRow, fieldType), field.type)
           && read_at(blob, at + offsetof(FieldRow, presence), field.presence)
           && read_at(blob, at + offsetof(FieldRow, parameter2), field.parameter2)
           && read_at(blob, at + offsetof(FieldRow, nested), field.nested)
           && read_at(blob, at + offsetof(FieldRow, bias), field.bias)
           && read_at(blob, at + offsetof(FieldRow, width), field.width)
           && read_at(blob, at + offsetof(FieldRow, parameter3), field.parameter3)
           && read_at(blob, at + offsetof(FieldRow, parameter4), field.parameter4);
}

[[nodiscard]] bool
read_property(const Image& image, std::uint64_t at, Property& property) noexcept {
    return image.u32(at + offsetof(PropertyRow, nameHash), property.nameHash)
           && image.u32(at + offsetof(PropertyRow, memberType), property.memberType)
           && image.u32(at + offsetof(PropertyRow, byteOffset), property.byteOffset);
}

[[nodiscard]] bool
read_property(std::span<const std::byte> blob, std::size_t at, Property& property) noexcept {
    return read_at(blob, at + offsetof(PropertyRow, nameHash), property.nameHash)
           && read_at(blob, at + offsetof(PropertyRow, memberType), property.memberType)
           && read_at(blob, at + offsetof(PropertyRow, byteOffset), property.byteOffset);
}

/**
 * Resolves one relative package section and checks its class marker and extent.
 * @param present Receives false when the relative offset is zero.
 * @param body Receives the section body offset.
 * @param count Receives the row count.
 * @return False when the section is malformed.
 */
[[nodiscard]] bool package_section(std::span<const std::byte> blob,
                                   std::size_t at,
                                   std::uint32_t expected,
                                   std::size_t header,
                                   std::size_t stride,
                                   bool& present,
                                   std::size_t& body,
                                   std::uint64_t& count) noexcept {
    std::int64_t relative = 0;
    if (!read_at(blob, at, relative)) {
        return false;
    }
    present = relative != 0;
    if (!present) {
        return true;
    }
    const std::int64_t target = static_cast<std::int64_t>(at) + relative;
    std::uint32_t marker = 0;
    if (target < 8 || static_cast<std::uint64_t>(target) + header > blob.size()
        || !read_at(blob, static_cast<std::size_t>(target) - reader::layout::kClassWordSize, marker)
        || marker != expected) {
        return false;
    }
    body = static_cast<std::size_t>(target);
    if (!read_at(blob, body, count) || count > kMaximumSectionCount
        || body + header + static_cast<std::size_t>(count) * stride > blob.size()) {
        return false;
    }
    return true;
}

} // namespace

/**
 * Parses one bound reflection record: header, generic argument, codec and property blocks.
 * @param image Mapped executable.
 * @param address Record address from the slot index.
 * @param output Receives the record on success.
 * @param error Receives the refusal name on failure.
 * @return False when the record is malformed or unreadable.
 */
bool parse_native(const Image& image,
                  std::uint64_t address,
                  Metadata& output,
                  const char*& error) noexcept {
    try {
        Metadata record{};
        std::uint32_t length = 0;
        std::uint8_t genericFlag = 0;
        if (!image.u32(address, length)
            || !image.u32(address + kDefinitionHashAt, record.definitionHash)
            || !image.u32(address + kSizeAt, record.size)
            || !image.u8(address + kAlignmentAt, record.alignment)
            || !image.u8(address + kKindAt, record.kind)
            || !image.u8(address + kGenericFlagAt, genericFlag)) {
            error = "metadata_read";
            return false;
        }
        std::uint32_t marker = 0;
        for (const std::uint32_t candidate : kMarkerOffsets) {
            std::uint32_t value = 0;
            if (candidate + 4 <= length && image.u32(address + candidate, value)
                && value == kReflectionMarker) {
                marker = candidate;
                break;
            }
        }
        if (marker == 0) {
            error = "metadata_marker";
            return false;
        }
        if (genericFlag == kGenericFlag) {
            std::uint32_t genericMarker = 0;
            if (marker < 8 || !image.u32(address + marker - 8, genericMarker)
                || genericMarker != kGenericMarker
                || !image.u32(address + marker - 4, record.generic)) {
                error = "metadata_generic";
                return false;
            }
            record.hasGeneric = true;
        }
        std::uint64_t cursor = marker;
        while (cursor + 8 <= length) {
            std::uint32_t kind = 0;
            std::uint32_t count = 0;
            BlockLayout layout{};
            const std::uint64_t body = address + cursor + kNativeBlockBodyAt;
            if (!image.u32(address + cursor + kNativeBlockClassAt, kind)
                || !image.u32(body, count)) {
                error = "metadata_read";
                return false;
            }
            if (!block_layout(kind, layout)) {
                error = "metadata_block_unknown";
                return false;
            }
            const std::uint64_t extent =
                cursor + layout.header + static_cast<std::uint64_t>(count) * layout.stride;
            if (extent > length) {
                error = "metadata_block_extent";
                return false;
            }
            // The first block names the record's class; a codec block overrides it.
            if (cursor == marker || kind == kCodecBlock) {
                record.definitionClass = kind;
            }
            if (kind == kCodecBlock) {
                if (record.codec.present) {
                    error = "metadata_codec_repeated";
                    return false;
                }
                Codec& codec = record.codec;
                codec.present = true;
                if (!image.u32(body + offsetof(CodecBlock, ownHandle), codec.handle)
                    || !image.u32(body + offsetof(CodecBlock, originalSize), codec.original)
                    || !image.u32(body + offsetof(CodecBlock, serializedSize), codec.serialized)
                    || !image.u32(body + offsetof(CodecBlock, flags), codec.flags)
                    || !image.u32(body + offsetof(CodecBlock, array), codec.array)) {
                    error = "metadata_read";
                    return false;
                }
                codec.fields.resize(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (!read_field(image,
                                    body + kPackageCodecHeader
                                        + std::uint64_t{kFieldRowSize} * index,
                                    codec.fields[index])) {
                        error = "metadata_read";
                        return false;
                    }
                }
            } else if (kind == kPropertyBlock) {
                if (record.hasProperties) {
                    error = "metadata_properties_repeated";
                    return false;
                }
                record.hasProperties = true;
                if (!read_property(
                        image, body + offsetof(PropertyBlock, baseNameHash), record.base)) {
                    error = "metadata_read";
                    return false;
                }
                record.properties.resize(count);
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (!read_property(image,
                                       body + kPackagePropertyHeader
                                           + std::uint64_t{kPropertyRowSize} * index,
                                       record.properties[index])) {
                        error = "metadata_read";
                        return false;
                    }
                }
            }
            cursor = extent;
        }
        std::uint32_t tail = 0;
        if (cursor != length
            && !(cursor + 4 == length && image.u32(address + cursor, tail)
                 && tail == kReflectionMarker)) {
            error = "metadata_tail";
            return false;
        }
        output = std::move(record);
        return true;
    } catch (...) {
        error = "allocation";
        return false;
    }
}

/**
 * Parses one package metadata blob: header, own codec section and property section.
 * @param handle Package tag the blob was read from; its codec must name the same handle.
 * @param blob Complete package entry.
 * @param output Receives the record on success.
 * @param error Receives the refusal name on failure.
 * @return False when the blob is malformed.
 */
bool parse_package(std::uint32_t handle,
                   std::span<const std::byte> blob,
                   Metadata& output,
                   const char*& error) noexcept {
    try {
        Metadata record{};
        record.package = true;
        std::uint8_t genericFlag = 0;
        if (blob.size() < kPackageMinimumSize || !read_at(blob, kSizeAt, record.size)
            || !read_at(blob, kAlignmentAt, record.alignment)
            || !read_at(blob, kKindAt, record.kind)
            || !read_at(blob, kGenericFlagAt, genericFlag)) {
            error = "package_metadata_read";
            return false;
        }
        if (genericFlag == kGenericFlag) {
            error = "package_metadata_generic";
            return false;
        }
        bool present = false;
        std::size_t body = 0;
        std::uint64_t count = 0;
        if (!package_section(blob,
                             kPackageCodecAt,
                             kCodecBlock,
                             kPackageCodecHeader,
                             kFieldRowSize,
                             present,
                             body,
                             count)) {
            error = "package_metadata_section";
            return false;
        }
        if (present) {
            Codec& codec = record.codec;
            std::uint32_t own = 0;
            if (!read_at(blob, body + offsetof(CodecBlock, ownHandle), own)
                || !read_at(blob, body + offsetof(CodecBlock, originalSize), codec.original)
                || !read_at(blob, body + offsetof(CodecBlock, serializedSize), codec.serialized)
                || !read_at(blob, body + offsetof(CodecBlock, flags), codec.flags)
                || !read_at(blob, body + offsetof(CodecBlock, array), codec.array)) {
                error = "package_metadata_read";
                return false;
            }
            if (own != handle || codec.original != record.size) {
                error = "package_codec_identity";
                return false;
            }
            codec.present = true;
            codec.fields.resize(static_cast<std::size_t>(count));
            for (std::size_t index = 0; index < codec.fields.size(); ++index) {
                if (!read_field(blob,
                                body + kPackageCodecHeader + index * kFieldRowSize,
                                codec.fields[index])) {
                    error = "package_metadata_read";
                    return false;
                }
            }
        }
        if (!package_section(blob,
                             kPackagePropertiesAt,
                             kPropertyBlock,
                             kPackagePropertyHeader,
                             kPropertyRowSize,
                             present,
                             body,
                             count)) {
            error = "package_metadata_section";
            return false;
        }
        if (present) {
            record.hasProperties = true;
            if (!read_property(blob, body + offsetof(PropertyBlock, baseNameHash), record.base)) {
                error = "package_metadata_read";
                return false;
            }
            record.properties.resize(static_cast<std::size_t>(count));
            for (std::size_t index = 0; index < record.properties.size(); ++index) {
                if (!read_property(blob,
                                   body + kPackagePropertyHeader + index * kPropertyRowSize,
                                   record.properties[index])) {
                    error = "package_metadata_read";
                    return false;
                }
            }
        }
        output = std::move(record);
        return true;
    } catch (...) {
        error = "allocation";
        return false;
    }
}

bool Store::read_package(std::uint32_t tag,
                         std::uint32_t expectedClass,
                         std::vector<std::byte>& output) noexcept {
    std::uint32_t classId = 0;
    return reader::read_tag(source_, scratch_, tag, output, classId) && classId == expectedClass;
}

/**
 * Returns one cached record, reading the executable slot or the package blob on first use.
 * A package blob is hashed for the evidence as soon as its class is right, before parsing.
 */
const Metadata* Store::get(std::uint32_t handle, const char*& error) noexcept {
    const auto cached = entries_.find(handle);
    if (cached != entries_.end()) {
        return &cached->second;
    }
    try {
        Metadata record{};
        const auto slot = image_.slots().find(handle);
        if (slot != image_.slots().end()) {
            if (!parse_native(image_, slot->second, record, error)) {
                return nullptr;
            }
        } else {
            std::vector<std::byte> blob;
            if (!read_package(handle, kPackageMetadataClass, blob)
                || blob.size() < kPackageMinimumSize) {
                error = "package_metadata_class";
                return nullptr;
            }
            Digest digest{};
            if (!middleware::crypto::sha256::hash(blob, digest)) {
                error = "hash";
                return nullptr;
            }
            packageHashes_[handle] = digest;
            if (!parse_package(handle, blob, record, error)) {
                return nullptr;
            }
        }
        return &entries_.emplace(handle, std::move(record)).first->second;
    } catch (...) {
        error = "allocation";
        return nullptr;
    }
}

namespace {

/** Recursive body of property_guard; `guard` is -1 when no field covers the offset. */
[[nodiscard]] bool guard_at(Store& store,
                            std::uint32_t handle,
                            std::uint64_t offset,
                            std::size_t depth,
                            std::uint64_t base,
                            std::int64_t inherited,
                            std::int64_t& guard,
                            const char*& error) noexcept {
    if (depth > kMaximumDepth) {
        error = "codec_recursion";
        return false;
    }
    const Metadata* record = store.get(handle, error);
    if (record == nullptr) {
        return false;
    }
    const Codec& codec = record->codec;
    if (!codec.present) {
        guard = -1;
        return true;
    }
    std::uint64_t repeat = 1;
    if (codec.array != 0) {
        if (codec.fields.size() != 1 || codec.array > kMaximumRepeat) {
            error = "array_codec_unsupported";
            return false;
        }
        repeat = codec.array;
    }
    const std::uint64_t table = store.image().primitive_sizes();
    for (std::uint64_t index = 0; index < repeat; ++index) {
        for (std::size_t fieldIndex = 0; fieldIndex < codec.fields.size(); ++fieldIndex) {
            const Field& field = codec.fields[fieldIndex];
            const std::uint64_t start =
                codec.array != 0 ? index * field.offset : std::uint64_t{field.offset};
            const std::uint64_t bit = base + (codec.array != 0 ? index * field.bit : field.bit);
            std::uint64_t size = 0;
            if (field.type == kTypeNested) {
                const Metadata* nested = store.get(field.nested, error);
                if (nested == nullptr) {
                    return false;
                }
                if (!nested->codec.present) {
                    error = "nested_codec_absent";
                    return false;
                }
                size = nested->codec.original;
            } else if (field.type < kTypeLimit) {
                std::uint32_t primitive = 0;
                if (!store.image().u32(table + 4ULL * field.type, primitive)) {
                    error = "primitive_size_read";
                    return false;
                }
                size = primitive;
            } else {
                error = "primitive_size_unknown";
                return false;
            }
            if (start <= offset && offset < start + size) {
                const bool presence = field.presence != 0;
                if (field.type != kTypeNested) {
                    guard = presence ? static_cast<std::int64_t>(bit) : inherited;
                    return true;
                }
                return guard_at(store,
                                field.nested,
                                offset - start,
                                depth + 1,
                                bit + (presence ? 1U : 0U),
                                presence ? static_cast<std::int64_t>(bit) : inherited,
                                guard,
                                error);
            }
        }
    }
    guard = -1;
    return true;
}

} // namespace

bool property_guard(Store& store,
                    std::uint32_t handle,
                    std::uint64_t offset,
                    std::int64_t& guard,
                    const char*& error) noexcept {
    return guard_at(store, handle, offset, 0, 1, 0, guard, error);
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans::internal
