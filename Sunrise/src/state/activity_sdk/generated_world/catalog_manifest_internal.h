#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../../../middleware/content/packages/tables/activity_definition_reader.h"
#include "catalog_manifest.h"

namespace sunrise::state::activity_sdk::generated_world::manifest::internal {

/** Eight bytes identify generated SDK catalog files. */
constexpr std::array<char, 8> kMagic{'S', 'R', 'S', 'D', 'K', 'C', 'A', 'T'};
/** Every catalog version uses the same bounded scenario-name representation. */
constexpr std::size_t kDiskScenarioNameCapacity = 64;
/** Version one remains readable so existing scenario shards can seed a current rebuild. */
constexpr std::uint32_t kLegacyVersion = 1;
/** Activity records retain three source-field locators from this installed table. */
using middleware::content::packages::tables::kActivityDefinitionTableTag;
/** Three activity locators plus two exact-root locators bound each activity row. */
constexpr std::size_t kMaximumPackageLocatorsPerVariant = 5;
/** All candidate and union sets are retained rather than compressed or discarded. */
constexpr std::size_t kMaximumEvidenceTags =
    kMaximumActivityVariantRecords
    * (2U * kMaximumActivityRootRecords + 2U * kMaximumScenarioRecords);
/** Locator evidence remains tightly bounded by the native producer. */
constexpr std::size_t kMaximumPackageLocators =
    kMaximumActivityVariantRecords * kMaximumPackageLocatorsPerVariant;
/** Eight attempts bound unique sibling creation. */
constexpr std::size_t kTemporaryNameAttempts = 8;
/** One dot separates each writer identity from the final name. */
constexpr std::wstring_view kComponentSeparator = L".";
/** Incomplete writer-owned files use this suffix. */
constexpr std::wstring_view kTemporarySuffix = L".tmp";
/** One byte takes two hexadecimal path characters. */
constexpr std::size_t kHexadecimalDigitsPerByte = 2;
/** A Windows DWORD takes eight hexadecimal path characters. */
constexpr std::size_t kWriterIdentifierDigits = sizeof(DWORD) * kHexadecimalDigitsPerByte;
/** Four bits select one hexadecimal digit. */
constexpr std::size_t kBitsPerHexadecimalDigit = 4;
/** The low four bits index the hexadecimal digit table. */
constexpr DWORD kHexadecimalDigitMask = (1UL << kBitsPerHexadecimalDigit) - 1UL;
/** Uppercase digits keep writer names stable and path-safe. */
constexpr std::wstring_view kHexadecimalDigits = L"0123456789ABCDEF";
#pragma pack(push, 1)

/** Short prefix permits an explicit version result before reading a version-specific header. */
struct Prefix final {
    std::array<char, kMagic.size()> magic{};
    std::uint32_t version{};
};

/** Fixed version-one prefix authenticating one bounded scenario section. */
struct LegacyHeader final {
    std::array<char, kMagic.size()> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint64_t fileSize{};
    std::uint32_t recordSize{};
    std::uint32_t recordCount{};
    std::uint32_t reserved{};
    Digest sourceFingerprint{};
    Digest payloadSha256{};
};

/** Fixed version-four prefix authenticating one identity and five bounded sections. */
struct Header final {
    std::array<char, kMagic.size()> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint64_t fileSize{};
    std::uint32_t scenarioRecordSize{};
    std::uint32_t scenarioRecordCount{};
    std::uint32_t activityRootRecordSize{};
    std::uint32_t activityRootRecordCount{};
    std::uint32_t activityVariantRecordSize{};
    std::uint32_t activityVariantRecordCount{};
    std::uint32_t evidenceTagRecordSize{};
    std::uint32_t evidenceTagRecordCount{};
    std::uint32_t packageLocatorRecordSize{};
    std::uint32_t packageLocatorRecordCount{};
    std::uint32_t reserved{};
    Digest sourceFingerprint{};
    Digest payloadSha256{};
};

/** Generation identity stored inside the hashed payload rather than only in the file header. */
struct DiskGenerationIdentity final {
    Digest sourceFingerprint{};
    Digest sdkBuildSha256{};
    Digest sdkPayloadSha256{};
};

/** Stable counted four-way partition independently recomputed by every reader. */
struct DiskBindingCompleteness final {
    std::uint32_t total{};
    std::uint32_t fixedScenario{};
    std::uint32_t namedDefinitionUnavailable{};
    std::uint32_t noDirectFixedActivityName{};
    std::uint32_t unresolvedRunnable{};
    std::uint8_t status{};
    std::array<std::uint8_t, 3> reserved{};
};

/** Stable fixed-width representation of one scenario shard. */
struct DiskScenarioRecord final {
    std::uint32_t scenarioTag{};
    std::uint8_t scenarioNameLength{};
    std::array<std::uint8_t, 3> reserved{};
    std::array<char, kDiskScenarioNameCapacity> scenarioName{};
    Digest shardPayloadSha256{};
};

/** Stable fixed-width representation of one activity root and both payload edges. */
struct DiskActivityRootRecord final {
    std::uint32_t activityRootTag{};
    std::uint32_t scenarioTag{};
    std::uint32_t transitionDescriptorTag{};
    std::uint8_t preferredNameLength{};
    std::uint8_t selectionStatus{};
    std::array<std::uint8_t, 2> reserved{};
    std::array<char, kActivityNameCapacity> preferredName{};
};

/** Stable fixed-width representation of one activity and four variable evidence spans. */
struct DiskActivityVariantRecord final {
    std::uint32_t activityIndex{};
    std::uint32_t definitionHash{};
    std::uint32_t activityRootTag{};
    std::uint32_t scenarioTag{};
    std::uint32_t matchmakingConfigTag{};
    std::uint32_t firstActivityRootCandidate{};
    std::uint32_t activityRootCandidateCount{};
    std::uint32_t firstScenarioNameCandidate{};
    std::uint32_t scenarioNameCandidateCount{};
    std::uint32_t firstEvidenceRootTag{};
    std::uint32_t evidenceRootTagCount{};
    std::uint32_t firstPackageLocator{};
    std::uint32_t packageLocatorCount{};
    std::uint8_t internalNameLength{};
    std::uint8_t joinStatus{};
    std::uint8_t bindingDisposition{};
    std::uint8_t bindingReason{};
    std::uint8_t bindingEvidenceBasis{};
    std::uint8_t runnableStatus{};
    std::uint8_t flags{};
    std::uint8_t reserved{};
    std::array<char, kActivityNameCapacity> internalName{};
};

/** Stable fixed-width representation of one exact package evidence locator. */
struct DiskPackageLocator final {
    std::uint32_t tag{};
    std::uint32_t reserved{};
    std::uint64_t offset{};
};

#pragma pack(pop)

/** Fixed packed disk sizes make producer and consumer ABI drift fail at compile time. */
inline constexpr std::size_t kPrefixSize = 12;
inline constexpr std::size_t kLegacyHeaderSize = 100;
inline constexpr std::size_t kHeaderSize = 132;
inline constexpr std::size_t kDiskGenerationIdentitySize = 96;
inline constexpr std::size_t kDiskBindingCompletenessSize = 24;
inline constexpr std::size_t kDiskScenarioRecordSize = 104;
inline constexpr std::size_t kDiskActivityRootRecordSize = 144;
inline constexpr std::size_t kDiskActivityVariantRecordSize = 188;
inline constexpr std::size_t kDiskPackageLocatorSize = 16;

static_assert(sizeof(Prefix) == kPrefixSize);
static_assert(sizeof(LegacyHeader) == kLegacyHeaderSize);
static_assert(sizeof(Header) == kHeaderSize);
static_assert(sizeof(DiskGenerationIdentity) == kDiskGenerationIdentitySize);
static_assert(sizeof(DiskBindingCompleteness) == kDiskBindingCompletenessSize);
static_assert(sizeof(DiskScenarioRecord) == kDiskScenarioRecordSize);
static_assert(sizeof(DiskActivityRootRecord) == kDiskActivityRootRecordSize);
static_assert(sizeof(DiskActivityVariantRecord) == kDiskActivityVariantRecordSize);
static_assert(sizeof(DiskPackageLocator) == kDiskPackageLocatorSize);
static_assert(build_data::scriptables::kScenarioNameCapacity == kDiskScenarioNameCapacity);
static_assert(std::is_trivially_copyable_v<Prefix> && std::is_standard_layout_v<Prefix>);
static_assert(std::is_trivially_copyable_v<LegacyHeader>
              && std::is_standard_layout_v<LegacyHeader>);
static_assert(std::is_trivially_copyable_v<Header> && std::is_standard_layout_v<Header>);
static_assert(std::is_trivially_copyable_v<DiskGenerationIdentity>
              && std::is_standard_layout_v<DiskGenerationIdentity>);
static_assert(std::is_trivially_copyable_v<DiskBindingCompleteness>
              && std::is_standard_layout_v<DiskBindingCompleteness>);
static_assert(std::is_trivially_copyable_v<DiskScenarioRecord>
              && std::is_standard_layout_v<DiskScenarioRecord>);
static_assert(std::is_trivially_copyable_v<DiskActivityRootRecord>
              && std::is_standard_layout_v<DiskActivityRootRecord>);
static_assert(std::is_trivially_copyable_v<DiskActivityVariantRecord>
              && std::is_standard_layout_v<DiskActivityVariantRecord>);
static_assert(std::is_trivially_copyable_v<DiskPackageLocator>
              && std::is_standard_layout_v<DiskPackageLocator>);

/** Maximum authenticated bytes accepted after the fixed header. */
constexpr std::uint64_t kMaximumPayloadSize =
    sizeof(DiskGenerationIdentity) + sizeof(DiskBindingCompleteness)
    + kMaximumScenarioRecords * sizeof(DiskScenarioRecord)
    + kMaximumActivityRootRecords * sizeof(DiskActivityRootRecord)
    + kMaximumActivityVariantRecords * sizeof(DiskActivityVariantRecord)
    + kMaximumEvidenceTags * sizeof(std::uint32_t)
    + kMaximumPackageLocators * sizeof(DiskPackageLocator);

/** Returns whether one digest is a usable nonzero generation identity. */
[[nodiscard]] bool valid_digest(const Digest& digest) noexcept;

/** Returns whether both runtime SDK digests are usable generation identities. */
[[nodiscard]] bool valid_sdk(const SdkIdentity& sdk) noexcept;

/** Copies one public scenario into its canonical zero-filled disk form. */
[[nodiscard]] bool encode_scenario(const ScenarioRecord& input,
                                   DiskScenarioRecord& output) noexcept;

/** Decodes one scenario only when all reserved and text bytes are canonical. */
[[nodiscard]] bool decode_scenario(const DiskScenarioRecord& input,
                                   ScenarioRecord& output) noexcept;

/** Copies one public activity root into its canonical zero-filled disk form. */
[[nodiscard]] bool encode_activity_root(const ActivityRootRecord& input,
                                        DiskActivityRootRecord& output) noexcept;

/** Decodes one activity root only when its disk representation is canonical. */
[[nodiscard]] bool decode_activity_root(const DiskActivityRootRecord& input,
                                        ActivityRootRecord& output) noexcept;

/** Copies one public activity variant and appends its canonical evidence sections. */
[[nodiscard]] bool encode_activity_variant(const ActivityVariantRecord& input,
                                           std::vector<std::uint32_t>& evidenceTags,
                                           std::vector<DiskPackageLocator>& packageLocators,
                                           DiskActivityVariantRecord& output) noexcept;

/** Decodes one activity variant only when all evidence spans are canonical and contiguous. */
[[nodiscard]] bool decode_activity_variant(const DiskActivityVariantRecord& input,
                                           std::span<const std::uint32_t> evidenceTags,
                                           std::span<const DiskPackageLocator> packageLocators,
                                           std::size_t& nextEvidenceTag,
                                           std::size_t& nextPackageLocator,
                                           ActivityVariantRecord& output) noexcept;

/** Orders activity variants by their stable catalog identity. */
[[nodiscard]] bool variant_less(const ActivityVariantRecord& left,
                                const ActivityVariantRecord& right) noexcept;

/** Encodes one bounded completeness summary into its canonical disk form. */
[[nodiscard]] bool encode_binding_completeness(const BindingCompleteness& input,
                                               DiskBindingCompleteness& output) noexcept;

/** Decodes one summary only when every enum and reserved byte is canonical. */
[[nodiscard]] bool decode_binding_completeness(const DiskBindingCompleteness& input,
                                               BindingCompleteness& output) noexcept;

/** Validates ordering, uniqueness, edges, evidence, and the reconstructed partition. */
[[nodiscard]] bool valid_catalog(std::span<const ScenarioRecord> scenarios,
                                 std::span<const ActivityRootRecord> roots,
                                 std::span<const ActivityVariantRecord> variants,
                                 BindingCompleteness* completeness = nullptr) noexcept;

} // namespace sunrise::state::activity_sdk::generated_world::manifest::internal
