#include "entity_object_type_build.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "../../../middleware/content/packages/schema/generated/tag_classes_80809b.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
namespace sunrise::client::content::activity::entity_object_types {
namespace {
namespace reader = middleware::content::packages::reader;
namespace types = state::gameplay::entity_object_types;
using Rsat = middleware::content::packages::schema::generated::SNetworkClassRecord;
using Definition = middleware::content::packages::schema::generated::SEntity;
/** The RSAT names its class definition, and the definition names the RSAT back. */
constexpr std::size_t kReverseOffset = offsetof(Rsat, ownerDefinition);
constexpr std::size_t kForwardOffset = offsetof(Definition, classRecordGameplayRoot);
constexpr std::size_t kObjectTypeOffset = offsetof(Definition, objectType);
struct Context {
    const reader::Source& source;
    reader::Scratch& scratch;
    types::Rows rows;
};
/** Joins one RSAT to its class definition and reads the definition's object type. */
bool collect(void* opaque, std::uint32_t rsat) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    try {
        if (context.rows.size() >= types::kMaximumRows) {
            return false;
        }
        std::vector<std::byte> resource, definition;
        std::uint32_t cls{}, backlink{}, forward{};
        if (!reader::read_tag(context.source, context.scratch, rsat, resource, cls)
            || cls != Rsat::kClass || resource.size() < kReverseOffset + sizeof backlink) {
            return false;
        }
        std::memcpy(&backlink, resource.data() + kReverseOffset, sizeof backlink);
        if (!backlink || backlink == 0xFFFFFFFFU
            || !reader::read_tag(context.source, context.scratch, backlink, definition, cls)
            || cls != Definition::kClass || definition.size() <= kObjectTypeOffset) {
            return false;
        }
        std::memcpy(&forward, definition.data() + kForwardOffset, sizeof forward);
        std::uint8_t objectType{};
        std::memcpy(&objectType, definition.data() + kObjectTypeOffset, sizeof objectType);
        if (forward != rsat || objectType > types::kMaximumObjectType) {
            return false;
        }
        context.rows.push_back({rsat, backlink, objectType});
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace
/** No partial or ambiguous package-class catalogue is published. */
bool build(const reader::Source& source,
           reader::Scratch& scratch,
           const types::Fingerprint& fingerprint) noexcept {
    try {
        Context context{source, scratch, {}};
        reader::ScanResult result{};
        if (!reader::scan_class(source, Rsat::kClass, &collect, &context, result)) {
            return false;
        }
        std::sort(context.rows.begin(), context.rows.end(), [](const auto& a, const auto& b) {
            return a.rsatTag < b.rsatTag;
        });
        return types::publish(std::move(context.rows), fingerprint);
    } catch (...) {
        return false;
    }
}
} // namespace sunrise::client::content::activity::entity_object_types
