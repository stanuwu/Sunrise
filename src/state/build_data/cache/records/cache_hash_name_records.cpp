#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {

/** Encodes one resolved bubble name. */
bool encode(const hash_names::Name& value, HashNameRecord& record) noexcept {
    record = {};
    if (value.nameLength == 0 || value.nameLength > hash_names::kNameLength) {
        return false;
    }
    record.hash = value.hash;
    record.nameLength = value.nameLength;
    std::copy(value.name.begin(), value.name.end(), record.name.begin());
    return true;
}

/** Decodes one resolved bubble name. */
bool decode(const HashNameRecord& record, hash_names::Name& value) noexcept {
    value = {};
    if (record.nameLength == 0 || record.nameLength > hash_names::kNameLength
        || record.reserved != decltype(record.reserved){}) {
        return false;
    }
    value.hash = record.hash;
    value.nameLength = record.nameLength;
    std::copy(record.name.begin(), record.name.end(), value.name.begin());
    return true;
}

} // namespace sunrise::state::build_data::cache::records
