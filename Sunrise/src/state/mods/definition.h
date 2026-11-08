#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../middleware/content/packages/schema/generated/tag_classes_808048.h"
#include "../content_manifest/definition.h"

namespace sunrise::state::mods {

/** Newest manifest format this build reads. A higher version needs a newer Sunrise. */
inline constexpr std::uint32_t kManifestVersion = 4;
/** New package ids a mod may own: the registrar floor up to the client header check's 0x0CFF. */
inline constexpr std::uint16_t kFirstOwnPackageId = content_manifest::kMinimumPackageId,
                               kLastOwnPackageId = 0x0CFF;
/** @return True for a package id a mod may own. */
[[nodiscard]] constexpr bool own_package_id(std::uint64_t id) noexcept {
    return id >= kFirstOwnPackageId && id <= kLastOwnPackageId;
}
/** Own package ids one manifest may list. */
inline constexpr std::size_t kOwnPackageLimit = 256;
/** Oldest manifest format still read. Version 1 has no `cheat` and no `config`. */
inline constexpr std::uint32_t kOldestManifestVersion = 1;
/** Mods one boot can hold. Caps the fixed State tables. */
inline constexpr std::size_t kModCapacity = 64;
/** A mod id is a short reverse-domain name. */
inline constexpr std::size_t kIdMaximumLength = 64;
/** Shortest id: two one-character segments and their dot. */
inline constexpr std::size_t kIdMinimumLength = 3;
/** The longest id and one spare byte, so a stored id is always null-terminated. */
inline constexpr std::size_t kIdCapacity = kIdMaximumLength + 1;
/** Display name bytes, UTF-8. */
inline constexpr std::size_t kNameCapacity = 64;
/** Description bytes, UTF-8. */
inline constexpr std::size_t kDescriptionCapacity = 512;
/** Bytes of one author entry, UTF-8. */
inline constexpr std::size_t kAuthorCapacity = 64;
/** Author entries one manifest may list. */
inline constexpr std::size_t kAuthorLimit = 8;
/** Source URL bytes. It is shown, never opened. */
inline constexpr std::size_t kSourceCapacity = 256;
/** Dependencies one manifest may list. */
inline constexpr std::size_t kDependencyLimit = 32;
/** Incompatible ids one manifest may list. */
inline constexpr std::size_t kIncompatibleLimit = 32;
/** Config entries one manifest may declare. The cap keeps the manifest fixed-size. */
inline constexpr std::size_t kConfigLimit = 32;
/** Bound scalar sites one mod may publish at startup. */
inline constexpr std::size_t kConfigBindingLimit = 256;
/** Largest entry a config value or a merged replacement may rewrite. */
inline constexpr std::uint32_t kConfigEntryByteLimit = 64U * 1024U * 1024U;
/** Config scalar profiles operate on CUI resource entries. */
inline constexpr std::uint32_t kConfigCuiResourceClass =
    middleware::content::packages::schema::generated::CuiResource::kClass;
/** Longest config key, in characters. */
inline constexpr std::size_t kConfigKeyMaximumLength = 32;
/** The longest key and one spare byte, so a stored key is always null-terminated. */
inline constexpr std::size_t kConfigKeyCapacity = kConfigKeyMaximumLength + 1;
/** A folder named after the longest id, and a null. */
inline constexpr std::size_t kFolderCapacity = kIdMaximumLength + 1;
/** Problems kept per mod; the first refusal takes priority over warnings. */
inline constexpr std::size_t kProblemLimit = 8;
/** Bytes of one problem message, UTF-8. */
inline constexpr std::size_t kProblemCapacity = 192;
/** Package files all mods may add to the manifest together. */
inline constexpr std::size_t kOverlayCapacity = 1024;
/** The client builds package paths in a 256-byte buffer, null included. */
inline constexpr std::size_t kClientPathCapacity = 256;
/** Absolute mod directory characters kept for script and SDK search paths. */
inline constexpr std::size_t kDirectoryCapacity = 512;

/** New packages in a mod folder. Every generation of each id ships together. */
inline constexpr std::wstring_view kPackagesFolder = L"packages";
/** Patches in a mod folder: the next generation of an installed package id. */
inline constexpr std::wstring_view kPatchesFolder = L"patches";
/** Lua modules a mod adds to the mission require path. */
inline constexpr std::wstring_view kSdkFolder = L"sdk";
/** Mission scripts a mod adds or replaces. */
inline constexpr std::wstring_view kScriptsFolder = L"scripts";

/** Bounded UTF-8 text kept in fixed State storage. */
template <std::size_t Capacity> struct Text final {
    std::array<char, Capacity> bytes{};
    std::size_t length{};

    /** @return The stored bytes. */
    [[nodiscard]] std::string_view view() const noexcept {
        return {bytes.data(), length};
    }
};

using Id = Text<kIdCapacity>;

/** A `MAJOR.MINOR.PATCH` version, each part 0..65535. */
struct Version final {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t patch{};
};

/** One required mod and the version range that satisfies it, both ends inclusive. */
struct Dependency final {
    Id id{};
    Version minimum{};
    Version maximum{};
    bool hasMaximum{};
};

/** The value type a config entry declares. */
enum class ConfigType : std::uint8_t {
    integer,
    floating,
    boolean,
};

/** One config value. Only the member named by `type` is meaningful. */
struct ConfigValue final {
    ConfigType type{ConfigType::integer};
    std::int32_t integer{};
    float floating{};
    bool boolean{};
};

/** One value a mod lets the user change on the Mods page. */
struct ConfigEntry final {
    Text<kConfigKeyCapacity> key{};
    /** Label on the Mods page. Empty means the page shows the key. */
    Text<kNameCapacity> name{};
    ConfigValue defaultValue{};
};

/** Entry layouts the config bake can patch. */
enum class ConfigBindingProfile : std::uint8_t {
    cuiProperty,
    cuiComparison,
};

/** One typed site in the exact plaintext entry supplied by a mod. */
struct ConfigBinding final {
    Text<kConfigKeyCapacity> key{};
    ConfigBindingProfile profile{ConfigBindingProfile::cuiProperty};
    ConfigType type{ConfigType::integer};
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint32_t size{};
    content_manifest::Fingerprint sourceSha256{};
    std::uint32_t propertyOffset{};
    std::uint32_t poolOffset{};
    std::uint32_t bindingOffset{};
    std::uint32_t offset{};
};

/** One parsed `mod.json`. */
struct Manifest final {
    std::uint32_t manifestVersion{};
    Id id{};
    Version version{};
    Text<kNameCapacity> name{};
    Text<kDescriptionCapacity> description{};
    std::array<Text<kAuthorCapacity>, kAuthorLimit> authors{};
    std::size_t authorCount{};
    Text<kSourceCapacity> source{};
    std::array<Dependency, kDependencyLimit> dependencies{};
    std::size_t dependencyCount{};
    std::array<Id, kIncompatibleLimit> incompatible{};
    std::size_t incompatibleCount{};
    /** A loaded cheat mod makes the session play on a copy of the save held in memory. */
    bool cheat{};
    std::array<ConfigEntry, kConfigLimit> config{};
    std::size_t configCount{};
    std::array<ConfigBinding, kConfigBindingLimit> configBindings{};
    std::size_t configBindingCount{};
    /** The new package ids the mod ships, ascending; its reference map names the same set. */
    std::array<std::uint16_t, kOwnPackageLimit> packages{};
    std::size_t packageCount{};
};

/** Why a mod did not load, or what it overrides. The log names the reason by `reason_name`. */
enum class Reason : std::uint8_t {
    manifestMissing,
    manifestUnreadable,
    manifestInvalid,
    manifestVersion,
    duplicateId,
    capacity,
    folderPath,
    missingDependency,
    dependencyVersion,
    dependencyNotLoaded,
    dependencyCycle,
    incompatible,
    packageInvalid,
    packageConflict,
    patchTarget,
    patchInvalid,
    mergeFailed,
    assetOverride,
    /** A warning: the mod's packages moved to free ids because another source holds them. */
    packageRemapped,
};

/** Refusals keep a mod out; warnings load it and say what it overrides. */
enum class Severity : std::uint8_t {
    refusal,
    warning,
};

/** One reason with its readable message. */
struct Problem final {
    Reason reason{};
    Severity severity{};
    Text<kProblemCapacity> message{};
};

/** A mod loads only when no refusal was recorded against it. */
enum class Status : std::uint8_t {
    loaded,
    refused,
};

/** One mod folder and everything the loader decided about it. */
struct Record final {
    Text<kFolderCapacity> folder{};
    Manifest manifest{};
    /** False when `mod.json` did not parse; the manifest fields are then empty. */
    bool manifestRead{};
    Status status{Status::refused};
    std::array<Problem, kProblemLimit> problems{};
    std::size_t problemCount{};
    /** Problems past the limit, counted so the list can say more exist. */
    std::size_t droppedProblems{};
    std::uint32_t newPackageFiles{};
    std::uint32_t patchFiles{};
    bool hasSdk{};
    bool hasScripts{};
};

/** One package file a mod adds to the client manifest, and where the client finds it. */
struct OverlayFile final {
    content_manifest::Row row{};
    /** Path relative to the game folder, ASCII, as the client's package path buffer takes it. */
    std::array<char, kClientPathCapacity> clientPath{};
    std::size_t clientPathLength{};
};

/** Borrowed mod list, valid only during one visit. Loaded mods come first, in load order. */
struct View final {
    const Record* records{};
    std::size_t count{};
    std::size_t loadedCount{};
    /** Package ids patched by more than one mod, merged into one generation. */
    std::size_t mergedPackages{};
    std::wstring_view modsDirectory;
};

/**
 * Visits one immutable mod list while State holds its shared lock.
 * @param context Caller context passed through without ownership.
 * @param view Borrowed records.
 * @return True when the caller consumed the view.
 */
using Visitor = bool (*)(void* context, const View& view) noexcept;

} // namespace sunrise::state::mods
