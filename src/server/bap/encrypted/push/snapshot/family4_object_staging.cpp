#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;

/** Logs which step of preparation failed. @return Always false. */
bool report_failure(const char* step) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=family4 stage=prepare result=fail step=%s", step);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Compresses one raw object and advances the shared sealed-buffer extent. */
bool append_object(Scratch& scratch,
                   std::span<const std::byte> encoded,
                   std::uint32_t definitionId,
                   std::uint64_t version,
                   middleware::queuez::Object& object,
                   std::size_t& compressedExtent) noexcept {
    std::size_t compressedSize = 0;
    if (!compress_object(
            scratch, encoded, definitionId, version, compressedExtent, object, compressedSize)) {
        return false;
    }
    compressedExtent += compressedSize;
    return true;
}

/** Encodes and appends one character's row-sorted item instances after any already staged. */
bool append_items(Scratch& scratch,
                  std::span<std::byte> rawStorage,
                  std::uint32_t itemInstanceObjectId,
                  const family4_datagen::loadout::ResolvedInstances& instances,
                  std::size_t baseIndex,
                  Prepared& staged,
                  std::size_t& itemCursor,
                  std::size_t& compressedExtent) noexcept {
    if (instances.itemCount > instances.items.size()
        || family4_datagen::instance::layout::kObjectSize > rawStorage.size()
        || baseIndex + itemCursor + instances.itemCount > staged.objects.size()) {
        return false;
    }

    const auto encoded = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    for (std::size_t itemIndex = 0; itemIndex < instances.itemCount; ++itemIndex) {
        const family4_datagen::instance::ResolvedInstance& instance =
            instances.items[itemIndex].instance;
        const std::size_t objectIndex = baseIndex + itemCursor;
        if (!family4_datagen::instance::encode(instance, encoded)) {
            return false;
        }
        if (!append_object(scratch,
                           encoded,
                           itemInstanceObjectId,
                           instance.instanceSoid,
                           staged.objects[objectIndex],
                           compressedExtent)) {
            return false;
        }
        ++itemCursor;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
