#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/** ClientRef slot type for actor lifecycle and named behavior channels. */
inline constexpr std::uint8_t kType2SlotType = 2;
inline constexpr std::uint32_t kType2ComponentClass = 0x8080834EU;
inline constexpr std::uint32_t kType2SenseSchema = 0x80807DA2U;
inline constexpr std::uint32_t kType2Schema = 0x80807DA1;
inline constexpr std::size_t kType2ChannelCapacity = 16;
inline constexpr std::size_t kType2TemperamentCapacity = 6;
/** Fixed bits around the variable 64-bit channel rows. */
inline constexpr std::size_t kType2ChannelBaseBitCount = 149;
inline constexpr std::size_t kType2ChannelRowBitCount = 64;
inline constexpr std::size_t kType2MaximumChannelBitCount =
    kType2ChannelBaseBitCount + kType2TemperamentCapacity * 32U
    + kType2ChannelCapacity * kType2ChannelRowBitCount;
/** Buffer the channel set needs at the full temperament and channel counts. */
inline constexpr std::size_t kType2MaximumChannelByteCount = (kType2MaximumChannelBitCount + 7) / 8;

/** One named float channel written into the actor object's property store on attach or restore. */
struct Type2Channel final {
    std::uint32_t nameHash{};
    float value{};
};

/** One package-generated actor temperament identity carried verbatim by Auth `.5.3`. */
struct TemperamentId final {
    std::uint32_t value{};

    /** @return True when both identities carry the same package value. */
    [[nodiscard]] friend constexpr bool operator==(TemperamentId, TemperamentId) noexcept = default;
};

/** Selects automatic actor creation or creation by an authored scene. */
enum class Type2ActorBinding : std::int8_t {
    selfOwned = -1,
    squadMember = 1,
};

/** Complete retained actor-channel set for one combatant ClientRef. */
struct Type2ChannelState final {
    std::array<TemperamentId, kType2TemperamentCapacity> temperaments{};
    std::array<Type2Channel, kType2ChannelCapacity> channels{};
    std::uint32_t revision{};
    std::uint8_t temperamentCount{};
    std::uint8_t count{};
    Type2ActorBinding actorBinding{Type2ActorBinding::selfOwned};
};

/** Exact 55-bit ClientRef used by keyed combatant lanes. */
struct Type2LaneClientRef final {
    std::uint32_t registryKey{0x811C9DC5U};
    std::int8_t slotType{};
    std::int16_t slotIndex{-1};
};

struct Type2LaneRefByte final {
    Type2LaneClientRef reference{};
    std::uint8_t value{};
};
struct Type2LaneAlternateRefByte final {
    Type2LaneClientRef reference{};
    std::uint8_t value{};
};
struct Type2LaneU32 final {
    std::uint32_t value{};
};
struct Type2LaneReal32 final {
    float value{};
};
struct Type2LaneRefByteBool final {
    Type2LaneClientRef reference{};
    std::uint8_t value{};
    bool enabled{};
};
struct Type2LaneEmpty final {};
struct Type2LaneU6 final {
    std::uint8_t value{};
};
struct Type2LaneU32Bool final {
    std::uint32_t value{};
    bool enabled{};
};
struct Type2LaneU32Real32 final {
    std::uint32_t value{};
    float real{};
};
struct Type2LaneTripleRef final {
    std::array<std::uint32_t, 3> values{};
    Type2LaneClientRef reference{};
    std::int8_t mode{-1};
    std::int8_t value{-128};
};

/** The ten schemas selected by the first keyed-lane tag. */
using Type2LanePrimary = std::variant<Type2LaneRefByte,
                                      Type2LaneU32,
                                      Type2LaneReal32,
                                      Type2LaneRefByteBool,
                                      Type2LaneEmpty,
                                      Type2LaneU6,
                                      Type2LaneU32Bool,
                                      Type2LaneU32Real32,
                                      Type2LaneAlternateRefByte,
                                      Type2LaneTripleRef>;

/** The third secondary schema stores the 11-bit quantized wire value directly. */
struct Type2LaneQuantized11 final {
    std::uint16_t value{};
};
enum class Type2LaneSecondaryEmpty : std::uint8_t { first, second };
using Type2LaneSecondary = std::variant<Type2LaneSecondaryEmpty, Type2LaneQuantized11>;

/** One complete 64-byte keyed lane selected by two finite schema tags. */
struct Type2KeyedLane final {
    Type2LanePrimary primary{};
    Type2LaneSecondary secondary{};
};

/** Buffer one keyed lane needs at its widest tag pair. */
inline constexpr std::size_t kType2KeyedLaneMaximumBitCount = 179;
inline constexpr std::size_t kType2KeyedLaneMaximumByteCount =
    (kType2KeyedLaneMaximumBitCount + 7) / 8;

/** The six-bit lane count caps the atom program. */
inline constexpr std::size_t kType2AtomCapacity = 32;
/** Generation, resume lane and count. The `.6` presence bit is already in the channel base. */
inline constexpr std::size_t kType2AtomBlockBitCount = 43;
/** Each stored lane carries its own presence bit ahead of the tagged body. */
inline constexpr std::size_t kType2AtomLaneMaximumBitCount = 1 + kType2KeyedLaneMaximumBitCount;

/**
 * The 32-lane client-atom program behind Auth `.6`. A generation change restarts it on the bound
 * actor. Generation zero leaves the whole block absent.
 */
struct Type2AtomProgram final {
    std::array<Type2KeyedLane, kType2AtomCapacity> lanes{};
    std::uint32_t generation{};
    std::uint8_t count{};
    /** Lane the runner resumes at, carried by `.6.1`. */
    std::uint8_t progressSeed{};
};

/** One complete type-2 root: the actor-control block and the atom program together. */
struct Type2Body final {
    Type2ChannelState channels{};
    Type2AtomProgram atoms{};
};

/** Buffer one whole type-2 root needs at the full channel and lane counts. */
inline constexpr std::size_t kType2MaximumBodyBitCount =
    kType2MaximumChannelBitCount + kType2AtomBlockBitCount
    + kType2AtomCapacity * kType2AtomLaneMaximumBitCount;
/** Same size padded up to whole bytes. */
inline constexpr std::size_t kType2MaximumBodyByteCount = (kType2MaximumBodyBitCount + 7) / 8;

/** Full Auth also permits spawn generation, eight keyed rows and eight manifest references. */
inline constexpr std::size_t kType2FullMaximumBitCount =
    kType2MaximumBodyBitCount + 31U + (1U + 8U * (1U + 32U + 1U + 31U) + 1U + 32U)
    + (4U + 8U * 55U + 31U);
inline constexpr std::size_t kType2FullMaximumByteCount = (kType2FullMaximumBitCount + 7U) / 8U;

/** Exact retained spans around `.6`; all other root fields remain byte-for-byte unchanged. */
struct Type2ProgramLayout final {
    std::size_t programOffset{};
    std::size_t programBits{};
    std::uint32_t generation{};
    std::uint8_t bindingWire{};
    bool enabled{};
};

/** Reads every full-root field using native bounded arrays and finite atom-lane tags. */
[[nodiscard]] bool inspect_type2_program(std::span<const std::byte> input,
                                         std::size_t bitCount,
                                         Type2ProgramLayout& output) noexcept;

/** Replaces only `.6`; a zero hash cancels the program on the enabled squad-bound actor. */
[[nodiscard]] bool replace_type2_sequence(std::span<const std::byte> previous,
                                          std::size_t previousBits,
                                          std::uint32_t sequenceHash,
                                          std::uint32_t committedGeneration,
                                          std::span<std::byte> output,
                                          std::size_t& written,
                                          std::size_t& writtenBits,
                                          std::uint32_t& generation) noexcept;

/** Encodes both keyed-lane tags and their selected native child schemas. */
[[nodiscard]] bool encode_type2_keyed_lane(const Type2KeyedLane& lane,
                                           std::span<std::byte> output,
                                           std::size_t& written,
                                           std::size_t& writtenBits) noexcept;

/** Writes the complete type-2 root with its control block and, when present, its atom program. */
[[nodiscard]] bool encode_type2_body(const Type2Body& body,
                                     std::span<std::byte> output,
                                     std::size_t& written,
                                     std::size_t& writtenBits) noexcept;

/** ClientRef slot type and Auth schema for the object-filter sensor. */
inline constexpr std::uint8_t kType34SlotType = 34;
inline constexpr std::uint32_t kType34ComponentClass = 0x80809568U;
inline constexpr std::uint32_t kType34Schema = 0x8080956AU;
inline constexpr std::size_t kType34PredicateCapacity = 8;

/**
 * Exact schemas accepted by the compact filter-predicate dispatcher. These are not the separate
 * 21-class authored behavior-condition registry.
 */
enum class Type34PredicateSchema : std::uint32_t {
    modeU32A = 0x80809571U,
    modeU32B = 0x80809572U,
    modeOnlyA = 0x80809573U,
    unregisteredModeSlotRef = 0x80809574U,
    modeU32C = 0x80809575U,
    modeFlagSlotRef = 0x80809576U,
    modeSlotRefA = 0x80809577U,
    modeSlotRefB = 0x80809578U,
    modeSlotRefC = 0x80809579U,
    modeI32 = 0x8080957AU,
    modeSlotRefD = 0x8080957BU,
    modeU32D = 0x8080957CU,
    modeOnlyB = 0x8080957DU,
};

struct Type34ModeU32A final {
    std::int8_t mode{-1};
    std::uint32_t value{};
};
struct Type34ModeU32B final {
    std::int8_t mode{-1};
    std::uint32_t value{};
};
struct Type34ModeOnlyA final {
    std::int8_t mode{-1};
};
struct Type34UnregisteredModeSlotRef final {
    std::int8_t mode{-1};
    Type2LaneClientRef reference{};
};
struct Type34ModeU32C final {
    std::int8_t mode{-1};
    std::uint32_t value{};
};
struct Type34ModeFlagSlotRef final {
    std::int8_t mode{-1};
    bool flag{};
    Type2LaneClientRef reference{};
};
struct Type34ModeSlotRefA final {
    std::int8_t mode{-1};
    Type2LaneClientRef reference{};
};
struct Type34ModeSlotRefB final {
    std::int8_t mode{-1};
    Type2LaneClientRef reference{};
};
struct Type34ModeSlotRefC final {
    std::int8_t mode{-1};
    Type2LaneClientRef reference{};
};
struct Type34ModeI32 final {
    std::int8_t mode{-1};
    std::int32_t value{};
};
struct Type34ModeSlotRefD final {
    std::int8_t mode{-1};
    Type2LaneClientRef reference{};
};
struct Type34ModeU32D final {
    std::int8_t mode{-1};
    std::uint32_t value{};
};
struct Type34ModeOnlyB final {
    std::int8_t mode{-1};
};

/** One selector plus one statically described predicate body. */
using Type34Predicate = std::variant<Type34ModeU32A,
                                     Type34ModeU32B,
                                     Type34ModeOnlyA,
                                     Type34UnregisteredModeSlotRef,
                                     Type34ModeU32C,
                                     Type34ModeFlagSlotRef,
                                     Type34ModeSlotRefA,
                                     Type34ModeSlotRefB,
                                     Type34ModeSlotRefC,
                                     Type34ModeI32,
                                     Type34ModeSlotRefD,
                                     Type34ModeU32D,
                                     Type34ModeOnlyB>;

struct Type34Body final {
    std::array<Type34Predicate, kType34PredicateCapacity> predicates{};
    std::uint8_t count{};
};

/** Buffer the predicate list needs at its widest child and the full predicate count. */
inline constexpr std::size_t kType34PredicateMaximumBitCount = 91;
inline constexpr std::size_t kType34MaximumBitCount =
    4 + kType34PredicateCapacity * kType34PredicateMaximumBitCount;
/** Same size padded up to whole bytes. */
inline constexpr std::size_t kType34MaximumByteCount = (kType34MaximumBitCount + 7) / 8;

/** Encodes one present code-34 field: presence, schema handle, and its native child body. */
[[nodiscard]] bool encode_type34_predicate(const Type34Predicate& predicate,
                                           std::span<std::byte> output,
                                           std::size_t& written,
                                           std::size_t& writtenBits) noexcept;

/** Encodes the complete type-34 Auth body containing zero to eight predicates. */
[[nodiscard]] bool encode_type34(const Type34Body& body,
                                 std::span<std::byte> output,
                                 std::size_t& written,
                                 std::size_t& writtenBits) noexcept;

/** Inserts or replaces one named channel without discarding the other retained rows. */
[[nodiscard]] bool
set_type2_channel(Type2ChannelState& state, std::uint32_t nameHash, float value) noexcept;

/** Selects one verified actor-ownership branch without changing retained actor controls. */
[[nodiscard]] bool set_type2_actor_binding(Type2ChannelState& state,
                                           Type2ActorBinding binding) noexcept;

/** Replaces the complete retained `.5.3` list with generated temperament identities. */
[[nodiscard]] bool set_type2_temperaments(Type2ChannelState& state,
                                          std::span<const TemperamentId> temperaments) noexcept;

/** Finds the next positive 31-bit actor-control revision without wrapping. */
[[nodiscard]] bool next_type2_revision(const Type2ChannelState& state,
                                       std::uint32_t& next) noexcept;

/** Encodes the exact type-2 body containing the retained channel set and no unrelated lanes. */
[[nodiscard]] bool encode_type2_channels(const Type2ChannelState& state,
                                         std::span<std::byte> output,
                                         std::size_t& written,
                                         std::size_t& writtenBits) noexcept;

/** Validates only the canonical retained actor-control body emitted above. */
[[nodiscard]] bool validate_type2_body(std::span<const std::byte> input,
                                       std::size_t bitCount) noexcept;

/** ClientRef slot type for an authored object entry. */
inline constexpr std::uint8_t kType4SlotType = 4;
inline constexpr std::uint32_t kType4Schema = 0x8080992F;
/** Canonical body with no transform override and no dynamic trailing entries. */
inline constexpr std::size_t kType4BitCount = 252;
inline constexpr std::size_t kType4ByteCount = (kType4BitCount + 7) / 8;

/** Last committed authored-object generation for one ClientRef. */
struct Type4GenerationGuard final {
    std::int32_t last{};
    bool hasLast{};
};

/** One authored object entry activation. The package owns the entry and its transform. */
struct Type4Preset final {
    std::int32_t generation{1};
    std::int32_t entryIndex{};
    bool active{true};
};

/** Finds the next positive authored-object generation without wrapping. */
[[nodiscard]] bool next_type4_generation(const Type4GenerationGuard& guard,
                                         std::int32_t& next) noexcept;

/** Encodes the canonical 252-bit authored-object activation body. */
[[nodiscard]] bool encode_type4(const Type4Preset& preset,
                                const Type4GenerationGuard& guard,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept;

/** Checks the canonical type-4 body used by the typed Host route. */
[[nodiscard]] bool validate_type4_body(std::span<const std::byte> input,
                                       std::size_t bitCount) noexcept;

/** ClientRef slot type for the objective reset body. */
inline constexpr std::uint8_t kType3SlotType = 3;
inline constexpr std::uint32_t kType3Schema = 0x80807F0C;
inline constexpr std::size_t kType3ObjectiveCount = 24;
/**
 * The root has two fields and both are presence-gated. So the body opens with the array's own
 * presence bit, before the 24 element pairs: 1 + 24 * (1 + 7) + 1 + 31.
 */
inline constexpr std::size_t kType3BitCount = 225;
inline constexpr std::size_t kType3ByteCount = (kType3BitCount + 7) / 8;
/** ClientRef slot type for the three-channel control body. */
inline constexpr std::uint8_t kType23SlotType = 23;
/** Runtime schema handle for the type-23 auth body. */
inline constexpr std::uint32_t kType23Schema = 0x80804F48;
/** Three repetitions of real32, signed int16, and bool. */
inline constexpr std::size_t kType23ChannelCount = 3;
inline constexpr std::size_t kType23BitCount = 147;
inline constexpr std::size_t kType23ByteCount = (kType23BitCount + 7) / 8;
/** ClientRef slot type for the configured trigger body. */
inline constexpr std::uint8_t kType31SlotType = 31;
/** Runtime schema handle for the type-31 auth body. */
inline constexpr std::uint32_t kType31Schema = 0x80809524;
/** One bool followed by two unsigned 64-bit values. */
inline constexpr std::size_t kType31BitCount = 129;
inline constexpr std::size_t kType31ByteCount = (kType31BitCount + 7) / 8;
/** ClientRef slot type for the replicated state carrying schema 0x80809919. */
inline constexpr std::uint8_t kType18SlotType = 18;
inline constexpr std::uint32_t kType18Schema = 0x80809919;
inline constexpr std::size_t kType18BitCount = 386;
inline constexpr std::size_t kType18ByteCount = (kType18BitCount + 7) / 8;
/** ClientRef slot type for the replicated countdown/timer body. */
inline constexpr std::uint8_t kType35SlotType = 35;
inline constexpr std::uint32_t kType35Schema = 0x808099BF;
inline constexpr std::size_t kType35BitCount = 359;
inline constexpr std::size_t kType35ByteCount = (kType35BitCount + 7) / 8;
/** ClientRef slot type for an authored sequence revision. */
inline constexpr std::uint8_t kType5SlotType = 5;
inline constexpr std::uint32_t kType5Schema = 0x80804F04;
inline constexpr std::size_t kType5BitCount = 7'359;
inline constexpr std::size_t kType5ByteCount = (kType5BitCount + 7) / 8;
/** ClientRef slot type for an authored cinematic state. */
inline constexpr std::uint8_t kType6SlotType = 6;
inline constexpr std::uint32_t kType6Schema = 0x80804F08;
/** Empty participant arrays are encoded by their zero counts, without trailing array storage. */
inline constexpr std::size_t kType6MinimumBitCount = 263;
inline constexpr std::size_t kType6MaximumBitCount = 1'671;
inline constexpr std::size_t kType6BitCount = kType6MinimumBitCount;
inline constexpr std::size_t kType6ByteCount = (kType6BitCount + 7) / 8;
/** ClientRef slot type for the authored task sensor generation. */
inline constexpr std::uint8_t kType38SlotType = 38;
inline constexpr std::uint32_t kType38Schema = 0x80807D89;
inline constexpr std::size_t kType38BitCount = 32;
inline constexpr std::size_t kType38ByteCount = 4;
/** ClientRef slot type for the authored dialogue sensor. */
inline constexpr std::uint8_t kType53SlotType = 53;
inline constexpr std::uint32_t kType53Schema = 0x80804F77;
/** The native schema serializes all 128 entries and gates each optional world id separately. */
inline constexpr std::size_t kType53EntryCount = 128;
inline constexpr std::size_t kType53MinimumBitCount = 19'767;
inline constexpr std::size_t kType53MaximumBitCount = 27'959;
/** One dialogue pulse has exactly one present 64-bit world id. */
inline constexpr std::size_t kType53BitCount = kType53MinimumBitCount + 64;
inline constexpr std::size_t kType53ByteCount = (kType53BitCount + 7) / 8;
/** ClientRef slot type for the three-lane authored HUD directive state. */
inline constexpr std::uint8_t kType68SlotType = 68;
inline constexpr std::uint32_t kType68Schema = 0x80804F67;
inline constexpr std::size_t kType68EntryCount = 3;
inline constexpr std::size_t kType68BitCount = 4'802;
inline constexpr std::size_t kType68ByteCount = (kType68BitCount + 7) / 8;
/** ClientRef slot type of an authored navigation marker a directive may point at. */
inline constexpr std::uint8_t kType47SlotType = 47;
/** ClientRef slot type of an authored volume an object filter may test against. */
inline constexpr std::uint8_t kType60SlotType = 60;
/** ClientRef slot type for the encounter/player engagement observer. */
inline constexpr std::uint8_t kType70SlotType = 70;
inline constexpr std::uint32_t kType70Schema = 0x808094F1;
/** Flags, two absent dynamic lists, and the signed 16-bit Sense revision. */
inline constexpr std::size_t kType70BitCount = 23;
inline constexpr std::size_t kType70ByteCount = (kType70BitCount + 7) / 8;

/** Native device-auth properties, matched against their consumers. */
enum class Type23Channel : std::uint8_t {
    devicePosition = 0,
    devicePower = 1,
    deviceLock = 2,
};

/** Last accepted type-23 sequence for each channel of one ClientRef. */
struct Type23SequenceGuard final {
    std::array<std::int16_t, kType23ChannelCount> last{};
};

/** One safe type-23 control preset. Unselected channels encode as inert sequence zero. */
struct Type23Preset final {
    Type23Channel channel{};
    float value{};
    std::int16_t sequence{1};
    bool snap{};
};

/** One reflected type-23 channel. */
struct Type23ChannelState final {
    float desiredValue{};
    std::int16_t sequence{};
    bool snap{};
};

/** Complete schema-shaped type-23 body. */
struct Type23Body final {
    std::array<Type23ChannelState, kType23ChannelCount> channels{};
};

/** One authored HUD element selected by its package name hash and bounded element index. */
struct Type68Preset final {
    std::uint32_t nameHash{};
    std::int32_t elementIndex{};
    /** Native directive state: 0 enters, 1 completes, and 2 uses the alternate exit state. */
    std::int8_t state{};
    bool visible{true};
    /** Authored type-47 destination; absent removes the explicit guidance marker. */
    Type2LaneClientRef navpoint{};
    /** Type-70 engagement sensor the client tests before it shows the mission banner. */
    Type2LaneClientRef audience{};
};

/** One exact type-70 state with both optional authored filter lists absent. */
struct Type70Preset final {
    /** Five raw native flag bits. The shipped constructor default is one. */
    std::uint8_t flags{1};
    /** Signed Sense-list revision. The shipped constructor default is one. */
    std::int16_t revision{1};
};

/** Encodes the complete three-lane type-68 body with one selected HUD element. */
[[nodiscard]] bool encode_type68(const Type68Preset& preset,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Validates the two native crash-bearing fields and the fixed type-68 body shape. */
[[nodiscard]] bool validate_type68_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Encodes the exact 23-bit type-70 body with no player or definition filter rows. */
[[nodiscard]] bool encode_type70(const Type70Preset& preset,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Validates the exact bounded no-filter type-70 body emitted by encode_type70. */
[[nodiscard]] bool validate_type70_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** ClientRef slot type for the performance sensor that starts one named actor performance. */
inline constexpr std::uint8_t kType42SlotType = 42;
inline constexpr std::uint32_t kType42ComponentClass = 0x80809583U;
inline constexpr std::uint32_t kType42Schema = 0x80809586U;
/** Present command group of name, value and generation; the optional row table stays absent. */
inline constexpr std::size_t kType42BitCount = 98;
inline constexpr std::size_t kType42ByteCount = (kType42BitCount + 7) / 8;

/** Last committed performance generation for one ClientRef. */
struct Type42GenerationGuard final {
    std::int32_t last{};
    bool hasLast{};
};

/** One performance start. The client fires it only while the generation rises. */
struct Type42Preset final {
    /** FNV-1 of a state name in the target actor's own state-machine definition. */
    std::uint32_t nameHash{};
    /** A second name hash. Every shipped package authors the empty-name basis here. */
    std::uint32_t value{0x811C9DC5U};
    std::int32_t generation{1};
};

/** Finds the next positive performance generation without wrapping. */
[[nodiscard]] bool next_type42_generation(const Type42GenerationGuard& guard,
                                          std::int32_t& next) noexcept;

/** Encodes the 98-bit type-42 body with the command group present and the row table absent. */
[[nodiscard]] bool encode_type42(const Type42Preset& preset,
                                 const Type42GenerationGuard& guard,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Validates the exact body emitted by encode_type42. */
[[nodiscard]] bool validate_type42_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** ClientRef slot type for the per-player public-event sensor. */
inline constexpr std::uint8_t kType71SlotType = 71;
inline constexpr std::uint32_t kType71Schema = 0x80804F57;
/** Biased int32 state, 64-bit player identity, one slot reference, one real32. */
inline constexpr std::size_t kType71BitCount = 183;
inline constexpr std::size_t kType71ByteCount = (kType71BitCount + 7) / 8;

/** Complete schema-shaped type-71 body. */
struct Type71Body final {
    /** Copied unchanged into the HUD directive that names this sensor. */
    std::int32_t state{};
    /** The message-5 player key. A different key re-arms the client's leave timer. */
    std::uint64_t playerIdentity{};
    /** The object whose zone list bounds the event area. */
    std::uint32_t areaRegistryKey{};
    std::uint8_t areaSlotType{};
    std::uint16_t areaSlotIndex{};
    /** Seconds the player may stay outside that area before the client latches Sense to 1. */
    float leaveSeconds{};
};

/** Encodes the fixed 183-bit type-71 body. */
[[nodiscard]] bool
encode_type71(const Type71Body& body, std::span<std::byte> output, std::size_t& written) noexcept;

/** Decodes one complete type-71 body. */
[[nodiscard]] bool decode_type71_body(std::span<const std::byte> input, Type71Body& body) noexcept;

/** Validates the exact type-71 body shape and its present area reference. */
[[nodiscard]] bool validate_type71_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Last committed objective reset generation for one ClientRef. */
struct Type3GenerationGuard final {
    std::int32_t last{};
    bool hasLast{};
};

/** One complete objective reset body. */
struct Type3Body final {
    std::array<std::int8_t, kType3ObjectiveCount> objectiveRevisions{};
    std::int32_t generation{1};
};

/** Finds the next positive objective reset generation without wrapping. */
[[nodiscard]] bool next_type3_generation(const Type3GenerationGuard& guard,
                                         std::int32_t& next) noexcept;

/** Encodes one complete objective reset body. */
[[nodiscard]] bool encode_type3(const Type3Body& body,
                                const Type3GenerationGuard& guard,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept;

/** Decodes one complete objective reset body. */
[[nodiscard]] bool decode_type3_body(std::span<const std::byte> input, Type3Body& body) noexcept;

/** Checks one complete objective reset body. */
[[nodiscard]] bool validate_type3_body(std::span<const std::byte> input,
                                       std::size_t bitCount) noexcept;

/** Last accepted type-31 generation for one ClientRef. */
struct Type31GenerationGuard final {
    std::uint64_t last{};
    bool hasLast{};
};

/** One safe type-31 pulse. Enabled is fixed true and the unconsumed auxiliary value is zero. */
struct Type31Preset final {
    std::uint64_t generation{};
};

/** Complete schema-shaped type-31 body. */
struct Type31Body final {
    bool enabled{};
    std::uint64_t value{};
    std::uint64_t auxiliary{};
};

/** Shared activity-timer state used by type 18 and type 35. */
struct SharedTimedState final {
    bool running{};
    std::uint64_t minimum{};
    std::uint64_t maximum{};
    std::uint64_t currentAtEpoch{};
    std::uint64_t remainingAtEpoch{};
    std::uint64_t epoch{};
    float rate{};
};

/** Complete schema-shaped type-18 body. */
struct Type18Body final {
    SharedTimedState timed{};
    bool tailFlag{};
    std::int32_t tailValue{};
};

/** Complete schema-shaped type-35 countdown/timer body. */
struct Type35Body final {
    bool flag0{};
    bool flag1{};
    /** Two-bit bias-one fields; their exact semantic labels remain unknown. */
    std::int8_t mode0{};
    std::int8_t mode1{};
    SharedTimedState timed{};
};

/** Last committed non-sentinel authored-sequence revision. */
struct Type5RevisionGuard final {
    std::uint8_t last{};
    bool hasLast{};
};

/** One authored sequence restart. */
struct Type5Preset final {
    std::uint8_t revision{1};
};

/** Last committed cinematic generation. */
struct Type6GenerationGuard final {
    std::uint32_t last{};
    bool hasLast{};
};

/** One authored cinematic start or stop transition. */
struct Type6Preset final {
    std::uint32_t generation{1};
    bool active{true};
};

/** Last committed task generation for one ClientRef. */
struct Type38GenerationGuard final {
    std::int32_t last{};
    bool hasLast{};
};

/** One exact authored-task change. */
struct Type38Preset final {
    std::int32_t generation{1};
};

/** Last committed fire sequence for every authored line of one dialogue sensor. */
struct Type53SequenceGuard final {
    std::array<std::int32_t, kType53EntryCount> last{};
};

/** One safe authored-dialogue pulse. The SDK bounds cueIndex to the authored list. */
struct Type53Preset final {
    std::uint16_t cueIndex{};
    std::int32_t sequence{1};
};

/** Finds the next positive per-channel type-23 sequence without wrapping. */
[[nodiscard]] bool next_type23_sequence(const Type23SequenceGuard& guard,
                                        Type23Channel channel,
                                        std::int16_t& next) noexcept;

/**
 * Encodes one canonical type-23 auth body without changing a refused output buffer.
 * The sequence must be positive and newer for the selected channel.
 */
[[nodiscard]] bool encode_type23(const Type23Preset& preset,
                                 const Type23SequenceGuard& guard,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Encodes one complete type-23 body without changing a refused output buffer. */
[[nodiscard]] bool encode_type23_body(const Type23Body& body,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

/** Decodes one exact type-23 body and checks its zero byte padding. */
[[nodiscard]] bool decode_type23_body(std::span<const std::byte> input, Type23Body& body) noexcept;

/** @return True when one packed override is a complete valid type-23 body. */
[[nodiscard]] bool validate_type23_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Finds the next type-31 generation without reaching its reserved maximum. */
[[nodiscard]] bool next_type31_generation(const Type31GenerationGuard& guard,
                                          std::uint64_t& next) noexcept;

/**
 * Encodes one canonical type-31 pulse without changing a refused output buffer.
 * The generation must be newer and must not equal the reserved maximum.
 */
[[nodiscard]] bool encode_type31(const Type31Preset& preset,
                                 const Type31GenerationGuard& guard,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Encodes one complete type-31 body without changing a refused output buffer. */
[[nodiscard]] bool encode_type31_body(const Type31Body& body,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

/** Decodes one exact type-31 body and checks its zero byte padding. */
[[nodiscard]] bool decode_type31_body(std::span<const std::byte> input, Type31Body& body) noexcept;

/** @return True when one packed override is a complete valid type-31 body. */
[[nodiscard]] bool validate_type31_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Encodes one complete type-18 body without changing a refused output buffer. */
[[nodiscard]] bool encode_type18_body(const Type18Body& body,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

/** Decodes one exact type-18 body and checks its zero byte padding. */
[[nodiscard]] bool decode_type18_body(std::span<const std::byte> input, Type18Body& body) noexcept;

/** @return True when one packed override is a complete valid type-18 body. */
[[nodiscard]] bool validate_type18_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Encodes one complete type-35 body without changing a refused output buffer. */
[[nodiscard]] bool encode_type35_body(const Type35Body& body,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

/** Decodes one exact type-35 body and checks its zero byte padding. */
[[nodiscard]] bool decode_type35_body(std::span<const std::byte> input, Type35Body& body) noexcept;

/** @return True when one packed override is a complete valid type-35 body. */
[[nodiscard]] bool validate_type35_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Finds the next sequence revision while reserving 0xFF as inactive. */
[[nodiscard]] bool next_type5_revision(const Type5RevisionGuard& guard,
                                       std::uint8_t& next) noexcept;

/** Encodes the complete canonical sequence body for one authored restart. */
[[nodiscard]] bool encode_type5(const Type5Preset& preset,
                                const Type5RevisionGuard& guard,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept;

/** Validates the canonical closed type-5 body accepted by this host. */
[[nodiscard]] bool validate_type5_body(std::span<const std::byte> input,
                                       std::size_t bitCount) noexcept;

/** Finds the next cinematic generation without wrapping. */
[[nodiscard]] bool next_type6_generation(const Type6GenerationGuard& guard,
                                         std::uint32_t& next) noexcept;

/** Encodes the canonical cinematic start/stop body with empty participant arrays. */
[[nodiscard]] bool encode_type6(const Type6Preset& preset,
                                const Type6GenerationGuard& guard,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept;

/** Validates the canonical closed type-6 body accepted by this host. */
[[nodiscard]] bool validate_type6_body(std::span<const std::byte> input,
                                       std::size_t bitCount) noexcept;

/** Finds the next positive task generation without wrapping. */
[[nodiscard]] bool next_type38_generation(const Type38GenerationGuard& guard,
                                          std::int32_t& next) noexcept;

/** Encodes the complete signed-int32 type-38 auth body. */
[[nodiscard]] bool encode_type38(const Type38Preset& preset,
                                 const Type38GenerationGuard& guard,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Decodes one exact type-38 body. */
[[nodiscard]] bool decode_type38_body(std::span<const std::byte> input,
                                      std::int32_t& generation) noexcept;

/** @return True when one packed override is a complete valid type-38 body. */
[[nodiscard]] bool validate_type38_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

/** Finds the next positive fire sequence for one authored dialogue cue without wrapping. */
[[nodiscard]] bool next_type53_sequence(const Type53SequenceGuard& guard,
                                        std::uint16_t cueIndex,
                                        std::int32_t& next) noexcept;

/** Encodes a complete 128-entry body with one present authored dialogue cue. */
[[nodiscard]] bool encode_type53(const Type53Preset& preset,
                                 const Type53SequenceGuard& guard,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

/** Validates the exact one-cue dialogue body and all unset-reference invariants. */
[[nodiscard]] bool validate_type53_body(std::span<const std::byte> input,
                                        std::size_t bitCount) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
