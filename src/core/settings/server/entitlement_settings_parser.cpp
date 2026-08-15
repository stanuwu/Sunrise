#include <algorithm>

#include "../../../state/entitlements/validation.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

/** Settings name for a definition that is published but never declared owned. */
constexpr std::string_view kOwnedNone = "none";
/** Settings name for a definition owned and declared by its manifest handle. */
constexpr std::string_view kOwnedHandle = "handle";
/** Settings name for a definition owned and declared by its application id. */
constexpr std::string_view kOwnedApplication = "application";

/**
 * Maps one settings ownership name onto its declaration form.
 * @param text Authored ownership name.
 * @return False for an unsupported name.
 */
[[nodiscard]] bool ownership_value(std::string_view text,
                                   state::entitlements::Ownership& output) noexcept {
    if (text == kOwnedNone) {
        output = state::entitlements::Ownership::none;
    } else if (text == kOwnedHandle) {
        output = state::entitlements::Ownership::handle;
    } else if (text == kOwnedApplication) {
        output = state::entitlements::Ownership::application;
    } else {
        return false;
    }
    return true;
}

} // namespace

/** Parses the authored entitlement array. */
bool Parser::entitlements(state::entitlements::Table& output) noexcept {
    output = {};
    if (!consume('[')) {
        return false;
    }
    if (consume(']')) {
        return true;
    }
    for (;;) {
        if (output.count >= output.entries.size() || !entitlement(output.entries[output.count])) {
            return false;
        }
        ++output.count;
        if (consume(']')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses one authored entitlement definition. */
bool Parser::entitlement(state::entitlements::Entitlement& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    bool hasName = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "name") {
            std::string_view name;
            if (hasName || !string(name) || name.empty()
                || name.size() >= state::entitlements::kNameCapacity) {
                return false;
            }
            std::copy(name.begin(), name.end(), output.name.begin());
            output.nameLength = static_cast<std::uint8_t>(name.size());
            hasName = true;
        } else if (key == "owned") {
            std::string_view owned;
            if (!string(owned) || !ownership_value(owned, output.ownership)) {
                return false;
            }
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return hasName && state::entitlements::valid(output);
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
