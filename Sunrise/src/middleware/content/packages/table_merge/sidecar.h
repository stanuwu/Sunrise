#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "../reader/layout.h"
#include "../schema/generated/tag_schema.h"
#include "generated/sidecar_format.h"
#include "table_merge.h"

namespace sunrise::middleware::content::packages::table_merge {

/** A merge sidecar sits beside its package under the same stem with this extension. */
inline constexpr std::wstring_view kSidecarFileExtension = L".merge";
/** Low bits of an op-local site target: the op index. */
inline constexpr std::uint16_t kOpLocalMask = kOpLocalTarget - 1U;
/** Bytes of a typed array descriptor {u64 count, i64 rel}. */
inline constexpr std::size_t kDescriptorSize = schema::generated::kTagArrayBytes;
/** Bytes of a row or object pointer, an i64 counted from its own field. */
inline constexpr std::size_t kRowPointerSize = schema::generated::kRelativePointerBytes;
/** Bytes of the package header digest a sidecar is bound to. */
using reader::layout::kMetadataDigestSize;
static_assert(field_size(FieldKind::ownedArray) == kDescriptorSize);
static_assert(field_size(FieldKind::rowPointer) == kRowPointerSize);
static_assert(field_size(FieldKind::objectPointer) == kRowPointerSize);
static_assert(kSidecarDigestSize == kMetadataDigestSize);

/** One row layout. */
struct Shape final {
    std::uint32_t stride{};
    std::vector<ShapeField> fields;
};

/** One stock range a mod changed. `old` points into the owning sidecar's file bytes. */
struct Diff final {
    std::uint32_t offset{};
    std::span<const std::byte> old;
};

/** One parsed entry description. */
struct Entry final {
    EntryHeader header{};
    std::vector<Diff> diffs;
    std::vector<OpRecord> ops;
    std::vector<SiteRecord> sites;
};

/** One parsed sidecar. It owns the file bytes its diffs point into, so it can move but not copy. */
class Sidecar final {
public:
    Sidecar() = default;
    Sidecar(const Sidecar&) = delete;
    Sidecar& operator=(const Sidecar&) = delete;
    Sidecar(Sidecar&&) noexcept = default;
    Sidecar& operator=(Sidecar&&) noexcept = default;

    /** @return The entry description for `index`, or null when the file has none. */
    [[nodiscard]] const Entry* entry(std::uint32_t index) const noexcept;
    [[nodiscard]] std::span<const TargetRecord> targets() const noexcept {
        return targets_;
    }
    /** @return Shape `index`, or null when out of range. */
    [[nodiscard]] const Shape* shape(std::uint32_t index) const noexcept {
        return index < shapes_.size() ? &shapes_[index] : nullptr;
    }
    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const FileHeader& header() const noexcept {
        return header_;
    }

private:
    friend Status parse(std::vector<std::byte> file,
                        std::uint16_t packageId,
                        std::uint16_t patchId,
                        std::span<const std::byte, kMetadataDigestSize> metadataDigest,
                        Sidecar& output) noexcept;

    std::vector<std::byte> file_;
    FileHeader header_{};
    std::vector<TargetRecord> targets_;
    std::vector<Shape> shapes_;
    std::vector<Entry> entries_;
};

/**
 * Parses and checks one sidecar against the package it sits next to.
 * @param file Whole file, moved into the output.
 * @param packageId Id of the package file.
 * @param patchId Generation of the package file.
 * @param metadataDigest The package header's metadata digest.
 * @param output Receives the parsed sidecar; left empty on failure.
 * @return ok, or why the file must be ignored.
 */
[[nodiscard]] Status parse(std::vector<std::byte> file,
                           std::uint16_t packageId,
                           std::uint16_t patchId,
                           std::span<const std::byte, kMetadataDigestSize> metadataDigest,
                           Sidecar& output) noexcept;

} // namespace sunrise::middleware::content::packages::table_merge
