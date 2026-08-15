#pragma once

#include <atomic>
#include <cstdint>

namespace sunrise::client::hooks::bootflow::spawn {

using NoArgPointer = void*(__fastcall*)() noexcept;
using PointerPredicate = bool(__fastcall*)(void*) noexcept;
using SliceSetIndexGetter = void*(__fastcall*)(void*, std::int32_t*) noexcept;
using DatumPointer = std::uint8_t*(__fastcall*)(std::int32_t) noexcept;
using OutPredicate = bool(__fastcall*)(std::int32_t*) noexcept;
using NoArgPredicate = bool(__fastcall*)() noexcept;

/** Every predicate the gate calls, found from the call operands inside its own body. */
struct Predicates {
    NoArgPointer sliceSetManager{};
    PointerPredicate worldPresent{};
    SliceSetIndexGetter currentSliceSet{};
    PointerPredicate sliceSetAddressable{};
    DatumPointer participationRecord{};
    OutPredicate lifetimeState{};
    NoArgPredicate worldControl{};
    NoArgPredicate hasAnyType13{};
    DatumPointer identity{};
    DatumPointer teamState{};
    DatumPointer teamBlock{};
    NoArgPredicate spawnSuppressed{};
};

/** The predicates, published only once every one of them decoded. */
inline Predicates g_calls{};
/** Guards g_calls: the probe reads nothing until the resolver publishes a complete set. */
inline std::atomic_bool g_ready{false};

/** Dumps the head of the participation record once per run, with the player key's own offset. */
void dump_record(const std::uint8_t* record) noexcept;

} // namespace sunrise::client::hooks::bootflow::spawn
