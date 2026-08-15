#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../../registry.h"
#include "../../signature_text.h"

namespace sunrise::client::patterns::game::config_getter {

/** Thunk the content state machine calls for its config URL. */
inline constexpr std::string_view kUrlGetterText = "E9 ? ? ? ? 48 87 2C 24 50 C3 CC E9 ? ? ? ? E9";

/** Thunk the content state machine calls for its config token. */
inline constexpr std::string_view kTokenGetterText =
    "E9 ? ? ? ? B8 FF FF FF FF E9 ? ? ? ? 48 03 45";

/** Config URL getter thunk signature byte count. */
inline constexpr std::size_t kUrlGetterPatternSize = signature_length(kUrlGetterText);
/** Config token getter thunk signature byte count. */
inline constexpr std::size_t kTokenGetterPatternSize = signature_length(kTokenGetterText);

extern constinit const std::array<patterns::PatternByte, kUrlGetterPatternSize> kUrlGetter;
extern constinit const std::array<patterns::PatternByte, kTokenGetterPatternSize> kTokenGetter;

} // namespace sunrise::client::patterns::game::config_getter
