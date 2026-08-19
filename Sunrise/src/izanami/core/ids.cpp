#include "ids.h"

namespace sunrise::izanami::core {

/** @return True when every UUID byte is zero. */
bool ForgeUUID::is_nil() const noexcept {
    for (const std::uint8_t byte : bytes) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

/** @return True when the resource contains at least one package identity token. */
bool ResourceId::is_valid() const noexcept {
    return classId != 0 || tagHash != 0 || wideHash != 0;
}

/** Builds a deterministic UUID from two 64-bit lanes. */
ForgeUUID make_forge_uuid(std::uint64_t high, std::uint64_t low) noexcept {
    ForgeUUID id;
    for (std::size_t index = 0; index < 8; ++index) {
        id.bytes[index] = static_cast<std::uint8_t>((high >> (index * 8)) & 0xFFU);
        id.bytes[index + 8] = static_cast<std::uint8_t>((low >> (index * 8)) & 0xFFU);
    }
    return id;
}

} // namespace sunrise::izanami::core