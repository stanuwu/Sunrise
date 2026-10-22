#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"
#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans {
namespace {

using internal::read_at;

// Matches the codec walk's primitive size lookup: movsx rcx, r9b; lea rdx, [table];
// mov edx, [rdx+rcx*4]. The table address comes from the wildcarded lea operand.
constexpr std::string_view kPrimitiveSizeLoadText =
    "49 0F BE C9 48 8D 15 ? ? ? ? 8B 14 8A 03 D3 3B D6";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kPrimitiveSizeLoad =
    patterns::signature<patterns::signature_length(kPrimitiveSizeLoadText)>(kPrimitiveSizeLoadText);
/** The lea operand and the end of the lea, from the start of the match. */
constexpr std::size_t kPrimitiveSizeOperand = 7;
constexpr std::size_t kPrimitiveSizeNext = 11;

/** PE32+ header layout the slot index depends on. */
constexpr std::size_t kNewHeaderOffsetAt = 0x3C;
constexpr std::size_t kSectionCountAt = 6;
constexpr std::size_t kOptionalSizeAt = 20;
constexpr std::size_t kOptionalHeaderAt = 24;
constexpr std::uint16_t kOptionalMagic64 = 0x20B;
constexpr std::size_t kImageBaseAt = 24;
constexpr std::size_t kSizeOfImageAt = 56;
/** The headers and section table of a mapped module fit in its first page. */
constexpr std::size_t kPeHeaderSpan = 4096;
constexpr std::size_t kSectionRowSize = 40;
constexpr std::size_t kSectionCharacteristicsAt = 36;
constexpr std::size_t kSectionLimit = 96;
/** A tagdefs slot is 32 bytes: handle, zero pad, record pointer, 16 more bytes. */
constexpr std::size_t kSlotSize = 32;
constexpr std::size_t kSlotStep = 8;
/** Every bound handle sits in the 0x8080xxxx space. */
constexpr std::uint32_t kHandleSpaceMask = 0xFFFF0000U;
constexpr std::uint32_t kHandleSpace = 0x80800000U;

} // namespace

/** Releases the view, the mapping and the file in that order. */
Image::~Image() noexcept {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
}

/**
 * Maps the executable file read-only and indexes the reflection slots.
 * @param path Null-terminated executable path, always mapped for the evidence digest.
 * @param module Base of the mapped module to read from, or null to read the file.
 * @return Status::ready when the headers parse and the slot index is complete.
 */
Status Image::open(const wchar_t* path, const void* module) noexcept {
    if (path == nullptr || path[0] == L'\0' || view_ != nullptr) {
        return Status::invalidInput;
    }
    file_ = CreateFileW(path,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        return Status::executableUnavailable;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file_, &size) == FALSE || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) > (std::numeric_limits<std::size_t>::max)()) {
        return Status::executableUnavailable;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
        return Status::executableUnavailable;
    }
    view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (view_ == nullptr) {
        return Status::executableUnavailable;
    }
    viewSize_ = static_cast<std::size_t>(size.QuadPart);
    data_ = module == nullptr ? view_ : static_cast<const std::byte*>(module);
    size_ = module == nullptr ? viewSize_ : kPeHeaderSpan;
    try {
        if (!parse_headers(module != nullptr) || !index_slots() || !resolve_primitive_sizes()) {
            return Status::executableInvalid;
        }
    } catch (...) {
        return Status::allocation;
    }
    return Status::ready;
}

std::span<const std::byte> Image::file() const noexcept {
    return view_ == nullptr ? std::span<const std::byte>{} : std::span(view_, viewSize_);
}

std::span<const std::byte> Image::bytes() const noexcept {
    return data_ == nullptr ? std::span<const std::byte>{} : std::span(data_, size_);
}

/**
 * Reads the PE32+ section table and the image base the slot pointers are relative to.
 * A mapped module keeps every section at its virtual address, so raw and virtual coincide there.
 * @param mapped True when the bytes are the module in memory.
 * @return False when the bytes are not a PE32+ image with a .vmp0 section.
 */
bool Image::parse_headers(bool mapped) noexcept {
    const std::span<const std::byte> image = bytes();
    std::uint32_t headerOffset = 0;
    std::uint16_t sectionCount = 0;
    std::uint16_t optionalSize = 0;
    std::uint16_t magic = 0;
    if (image.size() < 2 || image[0] != std::byte{'M'} || image[1] != std::byte{'Z'}
        || !read_at(image, kNewHeaderOffsetAt, headerOffset) || headerOffset > image.size() - 4
        || std::memcmp(image.data() + headerOffset, "PE\0\0", 4)
        || !read_at(image, headerOffset + kSectionCountAt, sectionCount)
        || !read_at(image, headerOffset + kOptionalSizeAt, optionalSize)
        || !read_at(image, headerOffset + kOptionalHeaderAt, magic) || magic != kOptionalMagic64
        || sectionCount == 0 || sectionCount > kSectionLimit
        || !read_at(image, headerOffset + kOptionalHeaderAt + kImageBaseAt, imageBase_)) {
        return false;
    }
    if (mapped) {
        std::uint32_t sizeOfImage = 0;
        if (!read_at(image, headerOffset + kOptionalHeaderAt + kSizeOfImageAt, sizeOfImage)) {
            return false;
        }
        // Slot pointers in memory are relocated, so the module base replaces the header's.
        imageBase_ = reinterpret_cast<std::uint64_t>(data_);
        size_ = sizeOfImage;
    }
    const std::size_t table = headerOffset + kOptionalHeaderAt + optionalSize;
    bool vmp0 = false;
    sections_.clear();
    for (std::size_t index = 0; index < sectionCount; ++index) {
        const std::size_t row = table + index * kSectionRowSize;
        std::uint32_t virtualSize = 0;
        std::uint32_t virtualAddress = 0;
        std::uint32_t rawSize = 0;
        std::uint32_t rawOffset = 0;
        std::uint32_t characteristics = 0;
        if (row > image.size() || image.size() - row < kSectionRowSize
            || !read_at(image, row + 8, virtualSize) || !read_at(image, row + 12, virtualAddress)
            || !read_at(image, row + 16, rawSize) || !read_at(image, row + 20, rawOffset)
            || !read_at(image, row + kSectionCharacteristicsAt, characteristics)) {
            return false;
        }
        const std::string_view name(reinterpret_cast<const char*>(image.data() + row), 8);
        const std::size_t nameLength = name.find('\0');
        const std::string_view trimmed = name.substr(0, nameLength);
        Section section{virtualAddress,
                        virtualSize,
                        mapped ? virtualAddress : rawOffset,
                        mapped ? virtualSize : rawSize,
                        trimmed == ".data",
                        (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0};
        if (trimmed == ".vmp0" && !vmp0) {
            vmp0 = true;
            vmp0Low_ = imageBase_ + virtualAddress;
            vmp0High_ = vmp0Low_ + virtualSize;
        }
        sections_.push_back(section);
    }
    return vmp0;
}

/**
 * Scans every .data section for 32-byte slots {handle, 0, record pointer into .vmp0}.
 * @return True; a section that runs past the file is clipped, not refused.
 */
bool Image::index_slots() noexcept {
    const std::span<const std::byte> image = bytes();
    slots_.clear();
    for (const Section& section : sections_) {
        if (!section.data || section.rawOffset >= image.size()) {
            continue;
        }
        const std::size_t length =
            static_cast<std::size_t>((std::min)(section.rawSize, image.size() - section.rawOffset));
        const std::span<const std::byte> data = image.subspan(section.rawOffset, length);
        for (std::size_t at = 0; at + kSlotSize < length; at += kSlotStep) {
            std::uint32_t handle = 0;
            std::uint32_t pad = 0;
            std::uint64_t record = 0;
            if (!read_at(data, at, handle) || !read_at(data, at + 4, pad)
                || !read_at(data, at + 8, record)) {
                break;
            }
            if (pad != 0 || (handle & kHandleSpaceMask) != kHandleSpace || record < vmp0Low_
                || record >= vmp0High_) {
                continue;
            }
            slots_.try_emplace(handle, record);
        }
    }
    return true;
}

/**
 * Finds the primitive size table through the one code site that indexes it.
 * @return False when the site is missing or not unique in the executable sections.
 */
bool Image::resolve_primitive_sizes() noexcept {
    const std::span<const std::byte> image = bytes();
    std::array<patterns::ImageRange, kSectionLimit> ranges{};
    std::array<const Section*, kSectionLimit> owners{};
    std::size_t count = 0;
    for (const Section& section : sections_) {
        if (!section.code || section.rawSize == 0 || section.rawOffset >= image.size()) {
            continue;
        }
        const std::size_t length =
            static_cast<std::size_t>((std::min)(section.rawSize, image.size() - section.rawOffset));
        // The scan only reads; the range type is mutable because hook targets share it.
        auto* const start = const_cast<std::byte*>(image.data() + section.rawOffset);
        ranges[count] = patterns::ImageRange{std::span(start, length)};
        owners[count++] = &section;
    }
    const std::array definitions{patterns::Pattern{"primitive_size_load", kPrimitiveSizeLoad}};
    std::array<patterns::Match, 1> matches{};
    if (!patterns::resolve_all(std::span(ranges.data(), count), definitions, matches)
        || matches[0].status != patterns::MatchStatus::unique) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::span<std::byte> range = ranges[index].bytes;
        if (matches[0].address < range.data()
            || matches[0].address >= range.data() + range.size()) {
            continue;
        }
        const auto inside = static_cast<std::uint64_t>(matches[0].address - range.data());
        std::int32_t displacement = 0;
        std::memcpy(&displacement, matches[0].address + kPrimitiveSizeOperand, sizeof displacement);
        const std::uint64_t site = imageBase_ + owners[index]->virtualAddress + inside;
        primitiveSizes_ = site + kPrimitiveSizeNext
                          + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement));
        return true;
    }
    return false;
}

/**
 * Maps one virtual address to a file offset through the section table.
 * @param address Virtual address relative to the image base in the header.
 * @param size Bytes the caller reads there.
 * @param at Receives the file offset.
 * @return False when no section holds the address or the read leaves the file.
 */
bool Image::offset(std::uint64_t address, std::size_t size, std::size_t& at) const noexcept {
    if (address < imageBase_) {
        return false;
    }
    const std::uint64_t relative = address - imageBase_;
    for (const Section& section : sections_) {
        if (relative < section.virtualAddress
            || relative - section.virtualAddress >= section.virtualSize) {
            continue;
        }
        const std::uint64_t inside = relative - section.virtualAddress;
        if (inside >= section.rawSize) {
            continue;
        }
        const std::uint64_t file = section.rawOffset + inside;
        if (file > size_ || size_ - file < size) {
            return false;
        }
        at = static_cast<std::size_t>(file);
        return true;
    }
    return false;
}

bool Image::u8(std::uint64_t address, std::uint8_t& value) const noexcept {
    std::size_t at = 0;
    return offset(address, sizeof value, at) && read_at(bytes(), at, value);
}

bool Image::u16(std::uint64_t address, std::uint16_t& value) const noexcept {
    std::size_t at = 0;
    return offset(address, sizeof value, at) && read_at(bytes(), at, value);
}

bool Image::u32(std::uint64_t address, std::uint32_t& value) const noexcept {
    std::size_t at = 0;
    return offset(address, sizeof value, at) && read_at(bytes(), at, value);
}

bool Image::i32(std::uint64_t address, std::int32_t& value) const noexcept {
    std::size_t at = 0;
    return offset(address, sizeof value, at) && read_at(bytes(), at, value);
}

bool Image::f32(std::uint64_t address, float& value) const noexcept {
    std::size_t at = 0;
    return offset(address, sizeof value, at) && read_at(bytes(), at, value);
}

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans
