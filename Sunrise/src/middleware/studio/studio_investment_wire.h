#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::middleware::studio::investment_wire {

/** Investment requests use their own local mapping. The magic reads "SSIV" in memory. */
inline constexpr std::uint32_t kMagic = 0x56495353;
/** Wire version. The game serves only a mapping whose editor wrote this exact version. */
inline constexpr std::uint32_t kVersion = 3;
/** Name of the mapping Studio creates for one game. The argument is the game's process id. */
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Investment.%lu";
/** How often the game looks for a missing editor mapping. */
inline constexpr std::uint64_t kConnectIntervalMs = 1'000;
/** An editor that has not refreshed its heartbeat this long is gone. */
inline constexpr std::uint64_t kHeartbeatTimeoutMs = 3'000;
/**
 * Size of the NUL-terminated ASCII reason carried by a refusal, an invalid request or a grant that
 * stopped part way.
 */
inline constexpr std::size_t kReasonBytes = 64;

/**
 * Studio and the game use these fixed offsets. Every version keeps the fields up to gamePid where
 * they are, so Studio can read gameVersion from a game that speaks another version.
 */
inline constexpr std::size_t kControlBytes = 48;
inline constexpr std::size_t kHeartbeatOffset = 16, kGameVersionOffset = 24, kGamePidOffset = 28;
inline constexpr std::size_t kGameStartedOffset = 32, kRequestOffset = 40, kResponseOffset = 44;
inline constexpr std::size_t kRequestBlockOffset = 48, kResponseBlockOffset = 128;
inline constexpr std::size_t kResponsePayloadOffset = 240;

// --- Capacities: the server asserts each one equals the State constant that owns it ---

/** The profile array's native slot count. */
inline constexpr std::size_t kProfileRowCapacity = 701;
/** Unequipped instances (350 native rows less 17 equipped) plus one per equipment slot. */
inline constexpr std::size_t kCharacterItemRecordCapacity = 333 + 17;
/** Stacks share the 333 unequipped character rows with instances. */
inline constexpr std::size_t kCharacterStackRecordCapacity = 333;
/** Every bucket id except the all-ones unavailable id. */
inline constexpr std::size_t kBucketRecordCapacity = 255;
/** Character records one hello can carry: an account's character slots. */
inline constexpr std::size_t kCharacterRecordCapacity = 3;
/** Rows one Season pass snapshot can carry: the pass catalog's reward rows. */
inline constexpr std::size_t kSeasonPassRowCapacity = 256;
/** Held pursuits are unequipped character instances. */
inline constexpr std::size_t kHeldPursuitCapacity = 333;
/** Objectives one held pursuit can carry: the objectives a pursuit contract can declare. */
inline constexpr std::size_t kPursuitObjectiveCapacity = 7;
/**
 * Sources one unlockValues request can carry. The fixed block offsets set this limit: the request
 * block ends where the response block begins, which leaves room after the request header for a
 * 4-byte count and nine 4-byte sources.
 */
inline constexpr std::size_t kUnlockReadCapacity = 9;
/** An item whose definition names no installed bucket. */
inline constexpr std::uint8_t kNoBucket = 0xFF;
/** A character item that occupies no equipment slot. */
inline constexpr std::uint8_t kUnequipped = 0xFF;

// --- Enumerations ---

/** The game serves every opcode between none and count. */
enum class Opcode : std::uint32_t {
    none,
    hello,
    inventorySnapshot,
    seasonPassSnapshot,
    pursuitSnapshot,
    grantItem,
    setQuantity,
    setSeasonalExperience,
    claimSeasonPassReward,
    resetSeasonPassClaim,
    readReceipt,
    acknowledgeReceipt,
    unlockValues,
    setPursuitObjective,
    completePursuitObjectives,
    count,
};

/**
 * Transport outcome of one request. A mutation's own outcome is in its receipt. Only an ok answer
 * carries a payload.
 */
enum class Status : std::uint32_t { none, ok, invalid, refused };

/** Unknown means the game holds no receipt with that id in this process. */
enum class ReceiptState : std::uint32_t {
    unknown,
    queued,
    deferred,
    refused,
    published,
    /**
     * The game saved the change but could not send its frame, so the client resyncs the account.
     * A grant ends at that step, and the rest of its quantity is never granted.
     */
    committedUnpublished,
};

/** Why a non-terminal operation is waiting. */
enum class Wait : std::uint32_t { none, publicationPeer, accountResync, catalog };

/** The saved row array a quantity edit names. */
enum class InventoryScope : std::uint8_t { profile, characterItem, characterStack };

/** What one source reads. Zero is never valid, so an unused source is all zero. */
enum class UnlockKind : std::uint8_t { flag = 1, value };

/** The saved bank one source reads. The profile bank holds only flags. */
enum class UnlockBank : std::uint8_t { account = 1, profile, character };

/** A source is invalid when its bank cannot hold that kind or its index is outside the bank. */
enum class UnlockReadState : std::uint8_t { invalid, known };

/** One bit per game catalog that has finished loading. */
enum Readiness : std::uint32_t {
    readyItems = 1,
    readyItemDetails = 2,
    readyBuckets = 4,
    readyProgressions = 8,
    readySeasonPass = 16,
    readyRewards = 32,
    readyObjectives = 64,
    readyPursuits = 128,
};

/** Bits of SeasonPassRow::flags: what the catalog and the saved state say about one row. */
enum SeasonPassRowFlags : std::uint16_t {
    rowAvailable = 1,
    rowClaimed = 2,
    rowAcquired = 4,
    rowAutomatic = 8,
    rowRankMet = 16,
    rowConditionKnown = 32,
    rowConditionMet = 64,
};

/** Bits of HeldPursuit::flags. A bounty is a pursuit with a lifetime. */
enum PursuitFlags : std::uint8_t {
    pursuitBounty = 1,
    pursuitObjectivesKnown = 2,
    pursuitObjectivesComplete = 4,
};

/** Bits of PursuitObjective::flags. Known means its value and target were read. */
enum ObjectiveFlags : std::uint8_t {
    objectiveKnown = 1,
    objectiveComplete = 2,
    objectiveSkipped = 4,
};

/** @return True for opcodes that submit an operation against an expected context. */
[[nodiscard]] constexpr bool is_mutation(Opcode opcode) noexcept {
    return (opcode >= Opcode::grantItem && opcode <= Opcode::resetSeasonPassClaim)
           || opcode == Opcode::setPursuitObjective || opcode == Opcode::completePursuitObjectives;
}

/** @return True for opcodes that name an earlier mutation's operation id. */
[[nodiscard]] constexpr bool is_receipt_request(Opcode opcode) noexcept {
    return opcode == Opcode::readReceipt || opcode == Opcode::acknowledgeReceipt;
}

// --- Identity block ---

/** The account, selected character and package content one request or answer is bound to. */
struct Context final {
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    /** The build-data content hash of the installed and mod packages. */
    std::uint64_t contentHash{};
};
static_assert(sizeof(Context) == 24);
static_assert(offsetof(Context, characterSoid) == 8 && offsetof(Context, contentHash) == 16);

// --- Request payloads ---

/**
 * quantity must be positive and no more than the account can hold of the item at once. For an
 * inventory item, that is a full stack in every row of its own bucket and of its recovery bucket.
 * A larger quantity is refused as "quantity".
 */
struct GrantItemRequest final {
    std::uint32_t itemHash{};
    std::uint32_t quantity{};
};
static_assert(sizeof(GrantItemRequest) == 8 && offsetof(GrantItemRequest, quantity) == 4);

/**
 * A quantity of zero removes the row. instanceSoid is zero for plain profile rows and character
 * stacks. A character row belongs to the expected context's character.
 */
struct SetQuantityRequest final {
    InventoryScope scope{};
    std::array<std::uint8_t, 7> reserved{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t expectedSerial{};
    std::int32_t expectedQuantity{};
    std::int32_t quantity{};
};
static_assert(sizeof(SetQuantityRequest) == 32);
static_assert(offsetof(SetQuantityRequest, reserved) == 1
              && offsetof(SetQuantityRequest, instanceSoid) == 8
              && offsetof(SetQuantityRequest, definitionHash) == 16
              && offsetof(SetQuantityRequest, expectedSerial) == 20
              && offsetof(SetQuantityRequest, expectedQuantity) == 24
              && offsetof(SetQuantityRequest, quantity) == 28);

/** Replaces the seasonal XP total, provided it still equals the total the editor read. */
struct SetSeasonalExperienceRequest final {
    std::int32_t expected{};
    std::int32_t replacement{};
};
static_assert(sizeof(SetSeasonalExperienceRequest) == 8
              && offsetof(SetSeasonalExperienceRequest, replacement) == 4);

/**
 * Names one pass row for a claim or a claim reset. rowIndex is the native claim index, and
 * itemHash must match the item that row grants.
 */
struct SeasonPassRowRequest final {
    std::uint16_t rowIndex{};
    std::uint16_t reserved{};
    std::uint32_t itemHash{};
};
static_assert(sizeof(SeasonPassRowRequest) == 8 && offsetof(SeasonPassRowRequest, reserved) == 2
              && offsetof(SeasonPassRowRequest, itemHash) == 4);

/** One saved flag or value, named by its bank and row. */
struct UnlockSource final {
    UnlockKind kind{};
    UnlockBank bank{};
    std::uint16_t index{};
};
static_assert(sizeof(UnlockSource) == 4 && offsetof(UnlockSource, bank) == 1
              && offsetof(UnlockSource, index) == 2);

/** Sources after count must be zero. */
struct UnlockValuesRequest final {
    std::uint16_t count{};
    std::uint16_t reserved{};
    std::array<UnlockSource, kUnlockReadCapacity> sources{};
};
static_assert(sizeof(UnlockValuesRequest) == 40);
static_assert(offsetof(UnlockValuesRequest, reserved) == 2
              && offsetof(UnlockValuesRequest, sources) == 4);

/**
 * Sets the saved row one objective of a held pursuit reads. expected is the value the editor read;
 * a flag reads 0 or 1.
 */
struct SetPursuitObjectiveRequest final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t expectedSerial{};
    std::uint32_t objectiveHash{};
    /** Position in the installed contract, below kPursuitObjectiveCapacity. */
    std::uint8_t ordinal{};
    std::uint8_t reserved{};
    std::uint16_t reserved2{};
    std::int32_t expected{};
    std::int32_t value{};
};
static_assert(sizeof(SetPursuitObjectiveRequest) == 32);
static_assert(offsetof(SetPursuitObjectiveRequest, itemHash) == 8
              && offsetof(SetPursuitObjectiveRequest, expectedSerial) == 12
              && offsetof(SetPursuitObjectiveRequest, objectiveHash) == 16
              && offsetof(SetPursuitObjectiveRequest, ordinal) == 20
              && offsetof(SetPursuitObjectiveRequest, reserved) == 21
              && offsetof(SetPursuitObjectiveRequest, reserved2) == 22
              && offsetof(SetPursuitObjectiveRequest, expected) == 24
              && offsetof(SetPursuitObjectiveRequest, value) == 28);

/** Sets every required objective of one held pursuit that is still incomplete to its target. */
struct CompletePursuitObjectivesRequest final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t expectedSerial{};
};
static_assert(sizeof(CompletePursuitObjectivesRequest) == 16);
static_assert(offsetof(CompletePursuitObjectivesRequest, itemHash) == 8
              && offsetof(CompletePursuitObjectivesRequest, expectedSerial) == 12);

/** Size in bytes of the request payload area, which holds the largest request. */
inline constexpr std::size_t kRequestPayloadBytes =
    (std::max)({sizeof(GrantItemRequest),
                sizeof(SetQuantityRequest),
                sizeof(SetSeasonalExperienceRequest),
                sizeof(SeasonPassRowRequest),
                sizeof(UnlockValuesRequest),
                sizeof(SetPursuitObjectiveRequest),
                sizeof(CompletePursuitObjectivesRequest)});
static_assert(kRequestPayloadBytes == 40);
// The unlock read is the largest request, so its capacity sets the payload size. The response
// block's fixed offset, asserted with the mapping below, keeps that payload from growing.
static_assert(sizeof(UnlockValuesRequest) == kRequestPayloadBytes);

/** @return The payload size a request with this opcode carries; zero for those with none. */
[[nodiscard]] constexpr std::size_t request_bytes(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::grantItem:
        return sizeof(GrantItemRequest);
    case Opcode::setQuantity:
        return sizeof(SetQuantityRequest);
    case Opcode::setSeasonalExperience:
        return sizeof(SetSeasonalExperienceRequest);
    case Opcode::claimSeasonPassReward:
    case Opcode::resetSeasonPassClaim:
        return sizeof(SeasonPassRowRequest);
    case Opcode::unlockValues:
        return sizeof(UnlockValuesRequest);
    case Opcode::setPursuitObjective:
        return sizeof(SetPursuitObjectiveRequest);
    case Opcode::completePursuitObjectives:
        return sizeof(CompletePursuitObjectivesRequest);
    default:
        return 0;
    }
}

// --- Response payloads: readers decode each counted prefix, and every later slot is zero ---

/** One of the live account's characters. selected is one for the selected character. */
struct CharacterEntry final {
    std::uint64_t soid{};
    std::uint8_t characterClass{};
    std::uint8_t selected{};
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(CharacterEntry) == 16);
static_assert(offsetof(CharacterEntry, characterClass) == 8
              && offsetof(CharacterEntry, selected) == 9 && offsetof(CharacterEntry, reserved) == 10
              && offsetof(CharacterEntry, reserved2) == 12);

/** The live account's characters and the catalogs the game has read. */
struct Hello final {
    /** Readiness bits. */
    std::uint32_t readiness{};
    std::uint8_t characterCount{};
    /** One when a signed-in game client bound to this account can show a change to it. */
    std::uint8_t peerOwnsAccount{};
    std::uint16_t reserved{};
    std::array<CharacterEntry, kCharacterRecordCapacity> characters{};
};
static_assert(sizeof(Hello) == 56);
static_assert(offsetof(Hello, characterCount) == 4 && offsetof(Hello, peerOwnsAccount) == 5
              && offsetof(Hello, reserved) == 6 && offsetof(Hello, characters) == 8);

/** One account-wide row of the profile. */
struct ProfileRow final {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    /** kNoBucket when the definition names no installed bucket. */
    std::uint8_t bucketId{};
    std::array<std::uint8_t, 3> reserved{};
};
static_assert(sizeof(ProfileRow) == 24);
static_assert(offsetof(ProfileRow, definitionHash) == 8 && offsetof(ProfileRow, quantity) == 12
              && offsetof(ProfileRow, mutationSerial) == 16 && offsetof(ProfileRow, bucketId) == 20
              && offsetof(ProfileRow, reserved) == 21);

/** One equipped or unequipped instance of the selected character. */
struct CharacterItem final {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint8_t bucketId{};
    /** The State equipment slot, or kUnequipped. */
    std::uint8_t equipmentSlot{};
    std::uint16_t reserved{};
};
static_assert(sizeof(CharacterItem) == 24);
static_assert(offsetof(CharacterItem, definitionHash) == 8
              && offsetof(CharacterItem, quantity) == 12
              && offsetof(CharacterItem, mutationSerial) == 16
              && offsetof(CharacterItem, bucketId) == 20
              && offsetof(CharacterItem, equipmentSlot) == 21
              && offsetof(CharacterItem, reserved) == 22);

/** One non-instanced stack of the selected character. */
struct CharacterStack final {
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint8_t bucketId{};
    std::array<std::uint8_t, 3> reserved{};
};
static_assert(sizeof(CharacterStack) == 16);
static_assert(offsetof(CharacterStack, quantity) == 4
              && offsetof(CharacterStack, mutationSerial) == 8
              && offsetof(CharacterStack, bucketId) == 12
              && offsetof(CharacterStack, reserved) == 13);

/** One installed bucket's row count and the equipment slot it fills. */
struct Bucket final {
    std::uint8_t bucketId{};
    /** -1 for a bucket that equips nothing. */
    std::int8_t equipmentSlot{};
    std::uint16_t slotCount{};
};
static_assert(sizeof(Bucket) == 4);
static_assert(offsetof(Bucket, equipmentSlot) == 1 && offsetof(Bucket, slotCount) == 2);

/** The selected character and the profile, read under one State lock. */
struct InventorySnapshot final {
    std::uint16_t profileCount{};
    /** Equipped and unequipped instances. */
    std::uint16_t itemCount{};
    std::uint16_t stackCount{};
    std::uint16_t bucketCount{};
    std::array<ProfileRow, kProfileRowCapacity> profile{};
    std::array<CharacterItem, kCharacterItemRecordCapacity> items{};
    std::array<CharacterStack, kCharacterStackRecordCapacity> stacks{};
    std::array<Bucket, kBucketRecordCapacity> buckets{};
    /** Pads the snapshot to its 8-byte alignment. */
    std::uint32_t reserved{};
};
static_assert(sizeof(InventorySnapshot) == 31'584);
static_assert(offsetof(InventorySnapshot, itemCount) == 2
              && offsetof(InventorySnapshot, stackCount) == 4
              && offsetof(InventorySnapshot, bucketCount) == 6
              && offsetof(InventorySnapshot, profile) == 8
              && offsetof(InventorySnapshot, items) == 16'832
              && offsetof(InventorySnapshot, stacks) == 25'232
              && offsetof(InventorySnapshot, buckets) == 30'560
              && offsetof(InventorySnapshot, reserved) == 31'580);

/** The row index is its array position, which is the native claim index. */
struct SeasonPassRow final {
    /** Zero for an unavailable placeholder row. */
    std::uint32_t itemHash{};
    std::uint8_t requiredRank{};
    std::uint8_t reserved{};
    /** SeasonPassRowFlags. */
    std::uint16_t flags{};
};
static_assert(sizeof(SeasonPassRow) == 8);
static_assert(offsetof(SeasonPassRow, requiredRank) == 4 && offsetof(SeasonPassRow, reserved) == 5
              && offsetof(SeasonPassRow, flags) == 6);

/** The saved seasonal XP and rank beside every catalog row's state. */
struct SeasonPassSnapshot final {
    std::int32_t experience{};
    std::uint16_t rank{};
    /** Catalog rows, including unavailable placeholders. */
    std::uint16_t rowCount{};
    std::array<SeasonPassRow, kSeasonPassRowCapacity> rows{};
};
static_assert(sizeof(SeasonPassSnapshot) == 2'056);
static_assert(offsetof(SeasonPassSnapshot, rank) == 4 && offsetof(SeasonPassSnapshot, rowCount) == 6
              && offsetof(SeasonPassSnapshot, rows) == 8);

/** One declared objective of a held pursuit with its evaluated value and target. */
struct PursuitObjective final {
    std::uint32_t objectiveHash{};
    std::int32_t value{};
    std::int32_t target{};
    /** ObjectiveFlags. */
    std::uint8_t flags{};
    std::array<std::uint8_t, 3> reserved{};
};
static_assert(sizeof(PursuitObjective) == 16);
static_assert(offsetof(PursuitObjective, value) == 4 && offsetof(PursuitObjective, target) == 8
              && offsetof(PursuitObjective, flags) == 12
              && offsetof(PursuitObjective, reserved) == 13);

/** One pursuit the selected character holds, with its declared objectives. */
struct HeldPursuit final {
    std::uint64_t instanceSoid{};
    std::uint32_t itemHash{};
    std::int32_t mutationSerial{};
    /** Native accumulated item-state bits. */
    std::uint32_t itemFlags{};
    /** PursuitFlags. */
    std::uint8_t flags{};
    std::uint8_t objectiveCount{};
    std::uint16_t reserved{};
    std::array<PursuitObjective, kPursuitObjectiveCapacity> objectives{};
};
static_assert(sizeof(HeldPursuit) == 136);
static_assert(offsetof(HeldPursuit, itemHash) == 8 && offsetof(HeldPursuit, mutationSerial) == 12
              && offsetof(HeldPursuit, itemFlags) == 16 && offsetof(HeldPursuit, flags) == 20
              && offsetof(HeldPursuit, objectiveCount) == 21
              && offsetof(HeldPursuit, reserved) == 22 && offsetof(HeldPursuit, objectives) == 24);

/** The selected character's held pursuits, evaluated against one saved state. */
struct PursuitSnapshot final {
    std::uint16_t heldCount{};
    std::uint16_t reserved{};
    std::uint32_t reserved2{};
    std::array<HeldPursuit, kHeldPursuitCapacity> held{};
};
static_assert(sizeof(PursuitSnapshot) == 45'296);
static_assert(offsetof(PursuitSnapshot, reserved) == 2 && offsetof(PursuitSnapshot, reserved2) == 4
              && offsetof(PursuitSnapshot, held) == 8);

/** One mutation's retained outcome. Both fields are zero for an unknown id. */
struct Receipt final {
    ReceiptState state{};
    Wait wait{};
};
static_assert(sizeof(Receipt) == 8 && offsetof(Receipt, wait) == 4);

/** What one source read, and whether the saved banks could answer it. */
struct UnlockValue final {
    UnlockReadState state{};
    std::uint8_t reserved{};
    std::uint16_t reserved2{};
    /** A flag reads 0 or 1. */
    std::int32_t value{};
};
static_assert(sizeof(UnlockValue) == 8);
static_assert(offsetof(UnlockValue, reserved) == 1 && offsetof(UnlockValue, reserved2) == 2
              && offsetof(UnlockValue, value) == 4);

/**
 * Value i answers request source i, all read from one saved state of the selected character.
 * Values past the request's count are zero.
 */
struct UnlockValues final {
    std::array<UnlockValue, kUnlockReadCapacity> values{};
};
static_assert(sizeof(UnlockValues) == 72);

/** Size in bytes of the response payload area, which holds the largest answer. */
inline constexpr std::size_t kResponsePayloadBytes = (std::max)({sizeof(Hello),
                                                                 sizeof(InventorySnapshot),
                                                                 sizeof(SeasonPassSnapshot),
                                                                 sizeof(PursuitSnapshot),
                                                                 sizeof(Receipt),
                                                                 sizeof(UnlockValues)});
static_assert(kResponsePayloadBytes == 45'296);

// --- The mapping ---

/** Studio writes the header, heartbeat and request id, and the game writes the rest. */
struct alignas(8) Control final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    /** Written whenever the magic matches, so Studio can explain a version mismatch. */
    std::uint32_t gameVersion{};
    std::uint32_t gamePid{};
    /** The game process's creation FILETIME. Receipts do not outlive the process. */
    std::uint64_t gameStarted{};
    alignas(4) volatile std::int32_t request{};
    alignas(4) volatile std::int32_t response{};
};
static_assert(sizeof(Control) == kControlBytes);
static_assert(offsetof(Control, version) == 4 && offsetof(Control, bytes) == 8
              && offsetof(Control, editorPid) == 12);
static_assert(offsetof(Control, heartbeat) == kHeartbeatOffset);
static_assert(offsetof(Control, gameVersion) == kGameVersionOffset);
static_assert(offsetof(Control, gamePid) == kGamePidOffset);
static_assert(offsetof(Control, gameStarted) == kGameStartedOffset);
static_assert(offsetof(Control, request) == kRequestOffset);
static_assert(offsetof(Control, response) == kResponseOffset);

/** Only Studio writes the request block. */
struct Request final {
    Opcode opcode{};
    /** Equals request_bytes(opcode). */
    std::uint32_t payloadBytes{};
    /** Nonzero exactly for mutations and receipt requests. */
    std::uint64_t operationId{};
    /** Complete for mutations, zero otherwise. */
    Context expected{};
    std::array<std::byte, kRequestPayloadBytes> payload{};
};
static_assert(sizeof(Request) == 80);
static_assert(offsetof(Request, payloadBytes) == 4 && offsetof(Request, operationId) == 8
              && offsetof(Request, expected) == 16 && offsetof(Request, payload) == 40);

/** Only the game writes the response block. */
struct Response final {
    Status status{};
    std::uint32_t reserved{};
    std::uint64_t operationId{};
    Opcode opcode{};
    /** The opcode's fixed payload size for an ok answer, else zero. */
    std::uint32_t payloadBytes{};
    /** The identity the payload is bound to, or zero for an invalid request. */
    Context context{};
    std::array<char, kReasonBytes> reason{};
    std::array<std::byte, kResponsePayloadBytes> payload{};
};
static_assert(offsetof(Response, reserved) == 4 && offsetof(Response, operationId) == 8
              && offsetof(Response, opcode) == 16 && offsetof(Response, payloadBytes) == 20
              && offsetof(Response, context) == 24 && offsetof(Response, reason) == 48
              && offsetof(Response, payload) == 112);

/** Each side writes its block in full before writing the id that announces it. */
struct alignas(8) SharedInvestment final {
    Control control;
    Request request;
    Response response;
};

/** Size in bytes of the whole mapping, which the editor's header must state. */
inline constexpr std::size_t kMappingBytes = kControlBytes + sizeof(Request) + sizeof(Response);
static_assert(offsetof(SharedInvestment, request) == kRequestBlockOffset);
static_assert(offsetof(SharedInvestment, response) == kResponseBlockOffset);
static_assert(kResponseBlockOffset + offsetof(Response, payload) == kResponsePayloadOffset);
static_assert(sizeof(SharedInvestment) == kMappingBytes && kMappingBytes == 45'536);
static_assert(std::is_standard_layout_v<SharedInvestment>);

} // namespace sunrise::middleware::studio::investment_wire
