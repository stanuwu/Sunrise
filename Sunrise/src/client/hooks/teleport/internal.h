#pragma once

#include <cstddef>
#include <cstdint>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::teleport {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Object-handle index bits. Two handles name the same object exactly when these bits match, so
 * comparing them is the whole ownership test. No datum array lookup is needed.
 */
inline constexpr std::uint32_t kHandleIndexMask = 0x1FFF;
/** The controlled-object getter writes this when the local player owns no object. */
inline constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;

/** Camera pose block stride, indexed by player. Its vectors are plain floats. */
inline constexpr std::size_t kCameraBlockStride = 0xC50;
inline constexpr std::size_t kCameraPositionX = 1428;
/** Camera forward vector. Its default is (1,0,0), so the basis is X forward, Z up. */
inline constexpr std::size_t kCameraForwardX = 1468;

/** Object handle the physics component drives, as a u16. */
inline constexpr std::size_t kPhysicsComponentObjectHandle = 44;
/** Non-zero here stops the sync before it reads anything else. */
inline constexpr std::size_t kPhysicsComponentSuppress = 568;

/** Body flag word the sync tests before it publishes a transform. */
inline constexpr std::size_t kBodyFlags = 76;
/** The bit in that word the sync requires. Clearing it skips the transform publish entirely. */
inline constexpr std::uint32_t kBodyActiveBit = 0x40;
/** Motion type. The sync excludes some values from publishing a transform. */
inline constexpr std::size_t kBodyMotionType = 352;
/** Rigid-body array on the physics component. */
inline constexpr std::size_t kPhysicsComponentBodyArray = 400;
/** Index into that array, signed. */
inline constexpr std::size_t kPhysicsComponentBodyIndex = 516;
/** Array entry stride, and the body pointer inside one entry. */
inline constexpr std::size_t kBodyEntryStride = 80;
inline constexpr std::size_t kBodyPointer = 32;

/**
 * Rigid-body world position. Still a hypothesis: it was lined up through a third-party Havok
 * layout, and the document meant to confirm it was never written.
 */
inline constexpr std::size_t kBodyPositionX = 448;
/** Rigid-body velocity. The sync copies this into the physics component every tick. */
inline constexpr std::size_t kBodyVelocityX = 560;

} // namespace sunrise::client::hooks::teleport
