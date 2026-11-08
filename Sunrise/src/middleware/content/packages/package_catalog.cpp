#include "package_catalog.h"

#include "../../../core/logging/log.h"
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
    if (named_tags::extract_directory(directory, false, &insert_mapping, nullptr, result)) {
        return true;
    }
    core::log::writef(core::log::Channel::middleware,
                      core::log::Level::error,
                      "ev=named_catalog stage=load result=fail files=%zu entries=%zu catalog=%zu",
                      result.files,
                      result.entries,
                      state::content::count());
    state::content::clear();
    result = {};
    return false;
}

} // namespace sunrise::middleware::content::packages
