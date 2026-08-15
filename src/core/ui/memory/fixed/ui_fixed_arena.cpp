#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>

#include "../allocator.h"
#include "internal.h"

namespace sunrise::core::ui::memory::fixed {
namespace {

/** Native maximum alignment covers every type Dear ImGui allocates. */
constexpr std::size_t kAllocationAlignment = alignof(std::max_align_t);
/** A split must leave one header and one aligned payload unit. */
constexpr std::size_t kMinimumSplitBytes = kAllocationAlignment;

/** Inline metadata tracks both physical neighbors and the free list. */
struct alignas(std::max_align_t) Block {
    std::size_t spanBytes{};
    std::size_t requestedBytes{};
    Block* previousPhysical{};
    Block* previousFree{};
    Block* nextFree{};
    bool free{};
};

static_assert(sizeof(Block) % kAllocationAlignment == 0);
static_assert(kArenaCapacityBytes % kAllocationAlignment == 0);

alignas(std::max_align_t) std::array<std::byte, kArenaCapacityBytes> g_storage{};
Block* g_freeHead{};
std::size_t g_outstandingAllocations{};
std::size_t g_outstandingBytes{};
std::size_t g_highWaterBytes{};

/** @return The byte count rounded up to the native maximum alignment. */
[[nodiscard]] constexpr std::size_t align_up(std::size_t value) noexcept {
    const std::size_t mask = kAllocationAlignment - 1;
    return (value + mask) & ~mask;
}

/** @return First payload byte after the block's aligned header. */
[[nodiscard]] std::byte* payload(Block* block) noexcept {
    return reinterpret_cast<std::byte*>(block) + sizeof(Block);
}

/** @return The next physical block, or null at the arena end. */
[[nodiscard]] Block* next_physical(Block* block) noexcept {
    std::byte* next = reinterpret_cast<std::byte*>(block) + block->spanBytes;
    const std::byte* end = g_storage.data() + g_storage.size();
    return next < end ? reinterpret_cast<Block*>(next) : nullptr;
}

/** @param block Free block to unlink from the free list. */
void remove_free(Block* block) noexcept {
    if (block->previousFree != nullptr) {
        block->previousFree->nextFree = block->nextFree;
    } else {
        g_freeHead = block->nextFree;
    }
    if (block->nextFree != nullptr) {
        block->nextFree->previousFree = block->previousFree;
    }
    block->previousFree = nullptr;
    block->nextFree = nullptr;
}

/** @param block Free block to put at the head of the free list. */
void insert_free(Block* block) noexcept {
    block->free = true;
    block->previousFree = nullptr;
    block->nextFree = g_freeHead;
    if (g_freeHead != nullptr) {
        g_freeHead->previousFree = block;
    }
    g_freeHead = block;
}

/**
 * Splits one free block, but never leaves an unusable tail fragment.
 * @param block Free block removed from the list by the caller.
 * @param requiredSpan Aligned header and payload span to consume.
 */
void split_block(Block* block, std::size_t requiredSpan) noexcept {
    const std::size_t remaining = block->spanBytes - requiredSpan;
    if (remaining < sizeof(Block) + kMinimumSplitBytes) {
        return;
    }
    block->spanBytes = requiredSpan;
    Block* remainder = ::new (reinterpret_cast<std::byte*>(block) + requiredSpan) Block{};
    remainder->spanBytes = remaining;
    remainder->previousPhysical = block;
    Block* following = next_physical(remainder);
    if (following != nullptr) {
        following->previousPhysical = remainder;
    }
    insert_free(remainder);
}

/**
 * Joins a released block with adjacent free spans before it re-enters the list.
 * @param block Owned block returned by Dear ImGui.
 */
void coalesce(Block* block) noexcept {
    Block* following = next_physical(block);
    if (following != nullptr && following->free) {
        remove_free(following);
        block->spanBytes += following->spanBytes;
        Block* afterFollowing = next_physical(block);
        if (afterFollowing != nullptr) {
            afterFollowing->previousPhysical = block;
        }
    }

    Block* merged = block;
    if (block->previousPhysical != nullptr && block->previousPhysical->free) {
        merged = block->previousPhysical;
        remove_free(merged);
        merged->spanBytes += block->spanBytes;
        Block* afterMerged = next_physical(merged);
        if (afterMerged != nullptr) {
            afterMerged->previousPhysical = merged;
        }
    }
    insert_free(merged);
}

/** @return Largest payload currently available from one free span. */
[[nodiscard]] std::size_t largest_free() noexcept {
    std::size_t largest = 0;
    for (Block* block = g_freeHead; block != nullptr; block = block->nextFree) {
        largest = (std::max)(largest, block->spanBytes - sizeof(Block));
    }
    return largest;
}

} // namespace

/** Rebuilds one complete free span after every prior allocation is released. */
void reset() noexcept {
    g_storage.fill(std::byte{});
    g_freeHead = ::new (g_storage.data()) Block{};
    g_freeHead->spanBytes = g_storage.size();
    g_freeHead->free = true;
    g_outstandingAllocations = 0;
    g_outstandingBytes = 0;
    g_highWaterBytes = 0;
}

/** Clears inactive arena bytes and lifecycle counters. */
void clear() noexcept {
    g_storage.fill(std::byte{});
    g_freeHead = nullptr;
    g_outstandingAllocations = 0;
    g_outstandingBytes = 0;
    g_highWaterBytes = 0;
}

/** Takes the first free span that holds one aligned request. */
void* allocate(std::size_t requestedBytes) noexcept {
    if (requestedBytes == 0 || requestedBytes > kArenaCapacityBytes - sizeof(Block)
        || requestedBytes
               > (std::numeric_limits<std::size_t>::max)() - (kAllocationAlignment - 1)) {
        return nullptr;
    }
    const std::size_t requiredSpan = sizeof(Block) + align_up(requestedBytes);
    Block* block = g_freeHead;
    while (block != nullptr && block->spanBytes < requiredSpan) {
        block = block->nextFree;
    }
    if (block == nullptr) {
        return nullptr;
    }

    remove_free(block);
    split_block(block, requiredSpan);
    block->free = false;
    block->requestedBytes = requestedBytes;
    ++g_outstandingAllocations;
    g_outstandingBytes += requestedBytes;
    g_highWaterBytes = (std::max)(g_highWaterBytes, g_outstandingBytes);
    return payload(block);
}

/** @param pointer Payload previously returned by allocate, or null. */
void release(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    Block* block = reinterpret_cast<Block*>(static_cast<std::byte*>(pointer) - sizeof(Block));
    if (block->free) {
        return;
    }
    --g_outstandingAllocations;
    g_outstandingBytes -= block->requestedBytes;
    block->requestedBytes = 0;
    coalesce(block);
}

/** @return Current counters and the largest merged free payload. */
ArenaStats snapshot() noexcept {
    return {g_outstandingAllocations,
            g_outstandingBytes,
            g_highWaterBytes,
            g_freeHead != nullptr ? largest_free() : 0};
}

} // namespace sunrise::core::ui::memory::fixed
