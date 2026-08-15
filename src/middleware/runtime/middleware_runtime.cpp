#include "middleware_runtime.h"

#include <Windows.h>

#include "../../core/filesystem/path.h"
#include "../../state/build_data/runtime.h"
#include "../content/packages/package_catalog.h"

namespace sunrise::middleware {
namespace {

/** Installed packages occupy the packages directory beside the main executable. */
constexpr std::wstring_view kPackageDirectorySuffix = L"packages";

} // namespace

/** Loads named package mappings on first boot when no complete State cache exists. */
bool initialize() noexcept {
    if (state::build_data::named_catalog_ready()) {
        return true;
    }
    core::path::Buffer packageDirectory;
    if (!core::path::module_directory(GetModuleHandleW(nullptr), packageDirectory)
        || !core::path::append(packageDirectory, kPackageDirectorySuffix)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(packageDirectory.chars.data());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        // A host without installed packages leaves first-boot generation pending.
        return true;
    }
    content::packages::named_tags::DirectoryResult result{};
    return content::packages::load_catalog(packageDirectory.chars.data(), result)
           && state::build_data::publish_named_catalog();
}

/** Releases no resources because State owns the generated content cache. */
void shutdown() noexcept {}

} // namespace sunrise::middleware
