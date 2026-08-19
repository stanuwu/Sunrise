#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "../lexer/token.h"

namespace sunrise::izanami::fate::parser {

struct EntityDeclaration {
    lexer::Token name{};
};

struct Module {
    std::vector<EntityDeclaration> entities{};
};

struct ParseResult {
    Module module{};
    bool ok{true};
};

class Parser final {
public:
    explicit Parser(std::span<const lexer::Token> tokens) noexcept;

    [[nodiscard]] ParseResult parse_module();

private:
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] const lexer::Token& peek() const noexcept;
    [[nodiscard]] const lexer::Token& advance() noexcept;

    std::span<const lexer::Token> tokens_{};
    std::size_t index_{};
};

} // namespace sunrise::izanami::fate::parser