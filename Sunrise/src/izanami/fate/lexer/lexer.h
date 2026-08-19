#pragma once

#include <string_view>

#include "token.h"

namespace sunrise::izanami::fate::lexer {

class Lexer final {
public:
    explicit Lexer(std::string_view source) noexcept;

    [[nodiscard]] Token next() noexcept;

private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] char peek() const noexcept;
    [[nodiscard]] char advance() noexcept;
    void skip_whitespace() noexcept;

    std::string_view source_{};
    std::size_t offset_{};
    std::uint32_t line_{1};
    std::uint32_t column_{1};
};

} // namespace sunrise::izanami::fate::lexer