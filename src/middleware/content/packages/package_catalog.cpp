#include "package_catalog.h"

#include "../../../state/content/content_catalog.h"

namespace sunrise::middleware::content::packages {
namespace {

/**
 * Copies one checked package mapping into fixed State storage.
 * @param context Unused visitor context.
 * @param entry Extracted named-definition entry.
 * @return True when State accepts the whole mapping.
 */
[[nodiscard]] bool insert_mapping([[maybe_unused]] void* context,
                                  const named_tags::Entry& entry) noexcept {
    return state::content::insert(
        std::string_view(entry.name.data(), entry.nameLength), entry.tag, entry.classId);
}

} // namespace

/** Replaces generated State mappings. The extraction is all or nothing. */
bool load_catalog(std::wstring_view directory, named_tags::DirectoryResult& result) noexcept {
    state::content::clear();
    if (named_tags::extract_directory(directory, &insert_mapping, nullptr, result)) {
        return true;
    }
    state::content::clear();
    result = {};
    return false;
}

} // namespace sunrise::middleware::content::packages
