#include "../../../../../middleware/compression/oodle/runtime.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Compresses one encoded family-four object into the next sealed scratch segment. */
bool compress_object(Scratch& scratch,
                     std::span<const std::byte> encoded,
                     std::uint32_t definitionId,
                     std::uint64_t version,
                     std::size_t destinationOffset,
                     middleware::queuez::Object& object,
                     std::size_t& written) noexcept {
    written = 0;
    if (destinationOffset > scratch.sealed.size()) {
        return false;
    }

    const auto destination = std::span(scratch.sealed).subspan(destinationOffset);
    std::size_t compressedSize = 0;
    if (!middleware::compression::oodle::compress_installed(encoded, destination, compressedSize)) {
        return false;
    }

    object = middleware::queuez::Object{
        definitionId,
        version,
        middleware::queuez::Encoding::oodle,
        destination.first(compressedSize),
    };
    written = compressedSize;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
