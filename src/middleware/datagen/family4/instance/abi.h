#pragma once

#include <cstddef>
#include <type_traits>

#include "layout.h"

namespace sunrise::middleware::datagen::family4::instance::abi {

/** The item instance SOID begins at native byte 0. */
inline constexpr std::size_t kInstanceSoidOffset = 0;
/** Item level begins after the 8-byte instance SOID. */
inline constexpr std::size_t kLevelOffset = 8;
/** The level-curve selector begins at native byte 12. */
inline constexpr std::size_t kLevelCurveOffset = 12;
/** The level-cap row begins at native byte 14. */
inline constexpr std::size_t kLevelCapRowOffset = 14;
/** The base item definition index begins at native byte 16. */
inline constexpr std::size_t kBaseDefinitionOffset = 16;
/** Ordinary socket rows begin at native byte 20. */
inline constexpr std::size_t kOrdinarySocketsOffset = 20;
/** The ordinary-socket active mask begins at native byte 164. */
inline constexpr std::size_t kActiveMaskOffset = 164;
/** The ordinary-socket blocked mask begins at native byte 172. */
inline constexpr std::size_t kBlockedMaskOffset = 172;
/** The server-provided socket gate mask begins at native byte 180. */
inline constexpr std::size_t kGateMaskOffset = 180;
/** Replicated item roll state begins at native byte 184. */
inline constexpr std::size_t kRollStateOffset = 184;
/** The optional progression definition begins at native byte 196. */
inline constexpr std::size_t kProgressionDefinitionOffset = 196;
/** 3-byte socket selectors begin at native byte 200. */
inline constexpr std::size_t kSelectorsOffset = 200;
/** The socket-entry-list row begins at native byte 236. */
inline constexpr std::size_t kSocketEntryListOffset = 236;
/** Opaque random-roll bytes begin at native byte 238. */
inline constexpr std::size_t kRandomRollOffset = 238;
/** Socket-entry state bytes begin at native byte 247. */
inline constexpr std::size_t kSocketEntryStatesOffset = 247;
/** The optional creation request begins at native byte 284. */
inline constexpr std::size_t kCreationRequestOffset = 284;
/** The creation-request entry count begins at native byte 304. */
inline constexpr std::size_t kCreationEntryCountOffset = 304;
/** Creation-request entries begin at native byte 308. */
inline constexpr std::size_t kCreationEntriesOffset = 308;
/** The optional tail definition begins at native byte 380. */
inline constexpr std::size_t kTailDefinitionOffset = 380;
/** Signed tail values begin at native byte 384. */
inline constexpr std::size_t kTailValuesOffset = 384;

static_assert(std::is_standard_layout_v<layout::Object>);
static_assert(offsetof(layout::Object, instanceSoid) == kInstanceSoidOffset);
static_assert(offsetof(layout::Object, level) == kLevelOffset);
static_assert(offsetof(layout::Object, level) + offsetof(layout::LevelState, curveX)
              == kLevelCurveOffset);
static_assert(offsetof(layout::Object, level) + offsetof(layout::LevelState, capRow)
              == kLevelCapRowOffset);
static_assert(offsetof(layout::Object, baseDefinitionIndex) == kBaseDefinitionOffset);
static_assert(offsetof(layout::Object, ordinarySockets) == kOrdinarySocketsOffset);
static_assert(offsetof(layout::Object, ordinarySockets)
                  + offsetof(layout::OrdinarySocketBlock, activeMask)
              == kActiveMaskOffset);
static_assert(offsetof(layout::Object, ordinarySockets)
                  + offsetof(layout::OrdinarySocketBlock, blockedMask)
              == kBlockedMaskOffset);
static_assert(offsetof(layout::Object, ordinarySockets)
                  + offsetof(layout::OrdinarySocketBlock, gateMask)
              == kGateMaskOffset);
static_assert(offsetof(layout::Object, roll) == kRollStateOffset);
static_assert(offsetof(layout::Object, roll)
                  + offsetof(layout::RollState, progressionDefinitionIndex)
              == kProgressionDefinitionOffset);
static_assert(offsetof(layout::Object, roll) + offsetof(layout::RollState, selectors)
              == kSelectorsOffset);
static_assert(offsetof(layout::Object, roll) + offsetof(layout::RollState, socketEntryListIndex)
              == kSocketEntryListOffset);
static_assert(offsetof(layout::Object, roll) + offsetof(layout::RollState, randomRoll)
              == kRandomRollOffset);
static_assert(offsetof(layout::Object, roll) + offsetof(layout::RollState, socketEntryStates)
              == kSocketEntryStatesOffset);
static_assert(offsetof(layout::Object, creation) == kCreationRequestOffset);
static_assert(offsetof(layout::Object, creation) + offsetof(layout::CreationRequest, entryCount)
              == kCreationEntryCountOffset);
static_assert(offsetof(layout::Object, creation) + offsetof(layout::CreationRequest, entries)
              == kCreationEntriesOffset);
static_assert(offsetof(layout::Object, tailDefinitionIndex) == kTailDefinitionOffset);
static_assert(offsetof(layout::Object, tailValues) == kTailValuesOffset);

} // namespace sunrise::middleware::datagen::family4::instance::abi
