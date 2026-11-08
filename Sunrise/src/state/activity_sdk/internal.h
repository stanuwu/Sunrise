#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "runtime.h"

namespace sunrise::state::activity_sdk {

/** @return True when one row reference belongs to the mapped section. */
template <typename Value>
[[nodiscard]] bool owns(std::span<const Value> values, const Value& value) noexcept {
    if (values.empty()) {
        return false;
    }
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(values.data());
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(&value);
    if (address < first) {
        return false;
    }
    const std::uintptr_t offset = address - first;
    return offset < values.size_bytes() && offset % sizeof(Value) == 0;
}

/** Loads the fixed module-relative pack against one catalog-authorized identity. */
[[nodiscard]] bool load(void* module,
                        const ExpectedIdentity& expected,
                        std::shared_ptr<Catalog>& output,
                        Status& result) noexcept;
/** Loads one explicit pack path against one independent expected identity. */
[[nodiscard]] bool load_path_expected(const wchar_t* path,
                                      const ExpectedIdentity& expected,
                                      std::shared_ptr<Catalog>& output,
                                      Status& result) noexcept;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** Loads the compile-pinned regression fixture in focused test binaries only. */
[[nodiscard]] bool
load_path(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept;
/** Loads a synthetic pack against one explicit payload pin in test binaries only. */
[[nodiscard]] bool load_path_for_test(const wchar_t* path,
                                      const std::array<std::byte, 32>& expectedPayloadSha256,
                                      std::shared_ptr<Catalog>& output,
                                      Status& result) noexcept;
#endif
/** Opens and maps one pack read-only; the caller validates it before publication. */
[[nodiscard]] bool
open_pack(const wchar_t* path, std::shared_ptr<Catalog>& output, Status& result) noexcept;
/** Checks every row and cross-section relation before publication. */
[[nodiscard]] bool valid_catalog(const Catalog& value) noexcept;
/** Checks the rows a mod pack carries: what resolve and the roster read, and nothing global. */
[[nodiscard]] bool valid_mod_catalog(const Catalog& value) noexcept;
/** Names the check the last refused catalog failed, with its row where the check is row-local. */
[[nodiscard]] const char* last_catalog_reason() noexcept;

} // namespace sunrise::state::activity_sdk
