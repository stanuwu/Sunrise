#include "seasonal_experience.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../investment/investment.h"
#include "../unlocks/definition.h"
#include "season_pass_reward_catalog.h"

namespace sunrise::state::progression::seasonal_experience {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\cache\\seasonal_experience.bin";
constexpr std::wstring_view kTemporarySuffix = L".tmp";
constexpr std::array<char, 8> kV1Magic{'S', 'N', 'R', 'S', 'X', 'P', '0', '1'};
constexpr std::array<char, 8> kV2Magic{'S', 'N', 'R', 'S', 'X', 'P', '0', '2'};
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '3'};
constexpr std::size_t kRewardCount = season_pass::kRewards.size();
constexpr std::size_t kRewardClaimByteCount = (kRewardCount + 7U) / 8U;
constexpr std::size_t kV1DocumentSize = kV1Magic.size() + sizeof(std::int32_t);
constexpr std::size_t kV2DocumentSize = kV1DocumentSize + kRewardClaimByteCount;
constexpr std::size_t kDocumentSize = kV2DocumentSize + sizeof(std::uint32_t);
constexpr std::int64_t kFirstArtifactPowerCost = 55'000;
constexpr std::int64_t kArtifactPowerCostStep = 110'000;
constexpr std::uint16_t kArtifactPowerBonusSlot = 602;
constexpr std::uint16_t kArtifactPointsUsedSlot = 604;
constexpr std::uint16_t kArtifactPointsEarnedSlot = 605;
constexpr std::array<std::int32_t, 12> kArtifactPointCosts{0,
                                                           40'000,
                                                           60'000,
                                                           100'000,
                                                           200'000,
                                                           246'000,
                                                           274'000,
                                                           420'000,
                                                           500'000,
                                                           600'000,
                                                           790'000,
                                                           900'000};
constexpr std::array<std::uint16_t, kArtifactSaleCount> kArtifactModFlags{
    1428, 1429, 1430, 1431, 1432, 0,    1393, 1394, 1395, 1396, 1397, 1388, 1389,
    1390, 1391, 1392, 1398, 1399, 1400, 1401, 1402, 1403, 1404, 1405, 1406, 1407};
/** Character acquired-flag mapping rows for each artifact vendor row. */
constexpr std::array<std::uint16_t, kArtifactSaleCount> kArtifactModCharacterRows{
    164, 165, 166, 167, 168, 0,   149, 150, 151, 152, 153, 144, 145,
    146, 147, 148, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163};
/** Character objective mapping row whose destination is artifact value slot 604. */
constexpr std::uint16_t kArtifactPointsUsedCharacterRow = 38;

std::mutex g_lock;
std::int32_t g_experience{};
std::array<std::uint8_t, kRewardClaimByteCount> g_rewardClaims{};
std::uint32_t g_artifactMods{};
core::path::Buffer g_path{};
bool g_pathReady{};
bool g_persistenceRequired{};

[[nodiscard]] constexpr std::uint16_t
artifact_power_bonus_for(std::int32_t experience) noexcept {
    std::int64_t remaining = (std::max)(experience, 0);
    std::uint16_t bonus = 1;
    std::int64_t nextCost = kFirstArtifactPowerCost;
    while (remaining >= nextCost && bonus < (std::numeric_limits<std::uint16_t>::max)()) {
        remaining -= nextCost;
        ++bonus;
        nextCost += kArtifactPowerCostStep;
    }
    return bonus;
}

static_assert(artifact_power_bonus_for(0) == 1);
static_assert(artifact_power_bonus_for(9'900'000) == 14);
static_assert(artifact_power_bonus_for(19'855'000) == 20);

[[nodiscard]] constexpr std::size_t reward_claim_byte(std::uint16_t rewardIndex) noexcept {
    return rewardIndex >> 3U;
}

[[nodiscard]] constexpr std::uint8_t reward_claim_mask(std::uint16_t rewardIndex) noexcept {
    return static_cast<std::uint8_t>(1U << (rewardIndex & 7U));
}

[[nodiscard]] std::uint16_t artifact_points_earned_locked() noexcept {
    std::int32_t remaining = g_experience;
    std::uint16_t points = 0;
    for (const std::int32_t cost : kArtifactPointCosts) {
        if (remaining < cost) {
            break;
        }
        remaining -= cost;
        ++points;
    }
    return points;
}

[[nodiscard]] std::uint16_t artifact_points_used(std::uint32_t artifactMods) noexcept {
    std::uint16_t points = 0;
    for (std::uint16_t row = 0; row < kArtifactModFlags.size(); ++row) {
        points += static_cast<std::uint16_t>((artifactMods & (1U << row)) != 0);
    }
    return points;
}

[[nodiscard]] std::uint16_t artifact_points_used_locked() noexcept {
    return artifact_points_used(g_artifactMods);
}

[[nodiscard]] bool
upsert_value(Family5State& family, std::uint16_t slot, std::int32_t value) noexcept {
    for (std::size_t index = 0; index < family.valueCount; ++index) {
        if (family.values[index].slot == slot) {
            family.values[index].value = value;
            return true;
        }
    }
    if (family.valueCount >= family.values.size()) {
        return false;
    }
    family.values[family.valueCount++] = UnlockValueOverride{slot, value};
    return true;
}

[[nodiscard]] bool project_artifact_state_locked(Family5State& family) noexcept {
    // Mod flags already travel in the character acquired-flag bank, including purchases and
    // resets. Do not spend 25 Family-5 rows repeating them. Remove authored copies too, so
    // stale overrides cannot mask the current character projection.
    std::size_t write = 0;
    for (std::size_t index = 0; index < family.flagCount; ++index) {
        const auto row = family.flags[index];
        // The table marks the sale row without a flag with 0, which must not strip an authored
        // slot-0 row.
        if (row.slot != 0
            && std::find(kArtifactModFlags.begin(), kArtifactModFlags.end(), row.slot)
                   != kArtifactModFlags.end()) {
            continue;
        }
        family.flags[write++] = row;
    }
    for (std::size_t index = write; index < family.flagCount; ++index) {
        family.flags[index] = {};
    }
    family.flagCount = write;
    return upsert_value(family, kArtifactPowerBonusSlot, artifact_power_bonus_for(g_experience))
           && upsert_value(family, kArtifactPointsUsedSlot, artifact_points_used_locked())
           && upsert_value(family, kArtifactPointsEarnedSlot, artifact_points_earned_locked());
}

[[nodiscard]] bool
project_artifact_character_state_locked(std::uint32_t artifactMods,
                                        std::span<std::byte> acquiredFlags,
                                        std::span<std::int32_t> objectiveValues) noexcept {
    if (acquiredFlags.size()
            <= *std::max_element(kArtifactModCharacterRows.begin(), kArtifactModCharacterRows.end())
        || objectiveValues.size() <= kArtifactPointsUsedCharacterRow) {
        return false;
    }
    for (std::uint16_t row = 0; row < kArtifactModCharacterRows.size(); ++row) {
        const std::uint16_t mapped = kArtifactModCharacterRows[row];
        if (mapped != 0) {
            acquiredFlags[mapped] = static_cast<std::byte>(
                (artifactMods & (1U << row)) != 0 ? unlocks::kFlagSet : unlocks::kFlagClear);
        }
    }
    objectiveValues[kArtifactPointsUsedCharacterRow] = artifact_points_used(artifactMods);
    return true;
}

[[nodiscard]] bool store_locked() noexcept {
    if (!g_pathReady) {
        return !g_persistenceRequired;
    }
    std::array<std::byte, kDocumentSize> document{};
    std::memcpy(document.data(), kMagic.data(), kMagic.size());
    std::memcpy(document.data() + kMagic.size(), &g_experience, sizeof g_experience);
    std::memcpy(document.data() + kMagic.size() + sizeof g_experience,
                g_rewardClaims.data(),
                g_rewardClaims.size());
    std::memcpy(document.data() + kV2DocumentSize, &g_artifactMods, sizeof g_artifactMods);
    core::path::Buffer temporaryPath = g_path;
    if (!core::path::append(temporaryPath, kTemporarySuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(temporaryPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool stored =
        WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr)
            != FALSE
        && written == document.size() && FlushFileBuffers(file) != FALSE;
    stored = CloseHandle(file) != FALSE && stored;
    stored = stored
             && MoveFileExW(temporaryPath.chars.data(),
                            g_path.chars.data(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                    != FALSE;
    if (!stored) {
        (void)DeleteFileW(temporaryPath.chars.data());
    }
    return stored;
}

void load_locked() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER fileSize{};
    const bool sized = GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart >= 0;
    std::array<std::byte, kDocumentSize> document{};
    DWORD read = 0;
    const bool readable =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size()), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    std::int32_t restored = 0;
    const bool current = sized && fileSize.QuadPart == static_cast<std::int64_t>(document.size())
                         && readable && read == document.size()
                         && std::memcmp(document.data(), kMagic.data(), kMagic.size()) == 0;
    const bool v2 = sized && fileSize.QuadPart == static_cast<std::int64_t>(kV2DocumentSize)
                    && readable && read == kV2DocumentSize
                    && std::memcmp(document.data(), kV2Magic.data(), kV2Magic.size()) == 0;
    const bool v1 = sized && fileSize.QuadPart == static_cast<std::int64_t>(kV1DocumentSize)
                    && readable && read == kV1DocumentSize
                    && std::memcmp(document.data(), kV1Magic.data(), kV1Magic.size()) == 0;
    if (!current && !v2 && !v1) {
        return;
    }
    std::memcpy(&restored, document.data() + kMagic.size(), sizeof restored);
    if (restored < 0) {
        return;
    }
    g_experience = restored;
    if (current || v2) {
        std::memcpy(g_rewardClaims.data(),
                    document.data() + kMagic.size() + sizeof g_experience,
                    g_rewardClaims.size());
    }
    if (current) {
        std::memcpy(&g_artifactMods, document.data() + kV2DocumentSize, sizeof g_artifactMods);
    }
}

} // namespace

bool initialize(void* module) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
    g_rewardClaims.fill(0);
    g_artifactMods = 0;
    g_path = {};
    g_pathReady = false;
    g_persistenceRequired = module != nullptr;
    if (!g_persistenceRequired) {
        return true;
    }
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kFileSuffix)) {
        return false;
    }
    g_pathReady = true;
    load_locked();
    return true;
}

void shutdown() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
    g_rewardClaims.fill(0);
    g_artifactMods = 0;
    g_path = {};
    g_pathReady = false;
    g_persistenceRequired = false;
}

bool grant(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    if (g_experience > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    const std::int32_t previous = g_experience;
    g_experience += amount;
    if (store_locked()) {
        return true;
    }
    g_experience = previous;
    return false;
}

std::int32_t earned() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_experience;
}

std::uint16_t rank() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::int32_t earnedRanks = g_experience / kExperiencePerRank;
    return static_cast<std::uint16_t>(
        (std::min)(static_cast<std::int32_t>(kMaximumRank), earnedRanks + 1));
}

std::uint16_t artifact_power_bonus() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return artifact_power_bonus_for(g_experience);
}

bool reward_claimed(std::uint16_t rewardIndex) noexcept {
    if (rewardIndex >= kRewardCount) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    return (g_rewardClaims[reward_claim_byte(rewardIndex)] & reward_claim_mask(rewardIndex)) != 0;
}

bool claim_reward(std::uint16_t rewardIndex) noexcept {
    if (rewardIndex >= kRewardCount) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::uint8_t mask = reward_claim_mask(rewardIndex);
    std::uint8_t& byte = g_rewardClaims[reward_claim_byte(rewardIndex)];
    if ((byte & mask) != 0) {
        return false;
    }
    const std::uint8_t previous = byte;
    byte = static_cast<std::uint8_t>(byte | mask);
    if (store_locked()) {
        return true;
    }
    byte = previous;
    return false;
}

bool apply_reward_claims(std::span<std::uint8_t> acquiredFlags) noexcept {
    if (acquiredFlags.size() <= season_pass::claim_account_flag_index(
            static_cast<std::uint16_t>(season_pass::kRewards.size() - 1U))) {
        return false;
    }

    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t index = 0; index < kRewardCount; ++index) {
        const auto rewardIndex = static_cast<std::uint16_t>(index);
        if ((g_rewardClaims[reward_claim_byte(rewardIndex)] & reward_claim_mask(rewardIndex))
            == 0) {
            continue;
        }
        acquiredFlags[season_pass::claim_account_flag_index(rewardIndex)] = unlocks::kFlagSet;
    }
    return true;
}

bool prepare_artifact_mod_unlock(std::uint16_t saleIndex,
                                 std::uint32_t& expected,
                                 std::uint32_t& replacement) noexcept {
    expected = 0;
    replacement = 0;
    if (saleIndex >= kArtifactModFlags.size() || kArtifactModFlags[saleIndex] == 0) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::uint32_t mask = 1U << saleIndex;
    const std::uint16_t used = artifact_points_used_locked();
    const std::uint16_t tierRequirement = saleIndex < 6    ? 0
                                          : saleIndex < 11 ? 1
                                          : saleIndex < 16 ? 4
                                          : saleIndex < 21 ? 7
                                                           : 10;
    if ((g_artifactMods & mask) != 0 || used >= artifact_points_earned_locked()
        || used < tierRequirement) {
        return false;
    }
    expected = g_artifactMods;
    replacement = g_artifactMods | mask;
    return true;
}

std::uint32_t artifact_mod_mask() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_artifactMods;
}

bool replace_artifact_mod_mask(std::uint32_t expected, std::uint32_t replacement) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    if (g_artifactMods != expected) {
        return false;
    }
    g_artifactMods = replacement;
    if (store_locked()) {
        return true;
    }
    g_artifactMods = expected;
    return false;
}

bool apply_artifact_state(Family5State& family) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    if (family.flagCount > family.flags.size() || family.valueCount > family.values.size()) {
        return false;
    }
    Family5State candidate = family;
    if (!project_artifact_state_locked(candidate)) {
        return false;
    }
    family = candidate;
    return true;
}

bool apply_artifact_character_state(std::span<std::byte> acquiredFlags,
                                    std::span<std::int32_t> objectiveValues) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return project_artifact_character_state_locked(
        g_artifactMods, acquiredFlags, objectiveValues);
}

bool apply_artifact_character_state(std::uint32_t artifactMask,
                                    std::span<std::byte> acquiredFlags,
                                    std::span<std::int32_t> objectiveValues) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return project_artifact_character_state_locked(
        artifactMask, acquiredFlags, objectiveValues);
}

} // namespace sunrise::state::progression::seasonal_experience
