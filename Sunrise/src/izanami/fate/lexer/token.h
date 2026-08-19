#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::izanami::fate::lexer {

enum class TokenKind : std::uint8_t {
    endOfFile,
    identifier,
    number,
    stringLiteral,
    leftBrace,
    rightBrace,
    leftParen,
    rightParen,
    colon,
    comma,
    dot,
    equal,
    keywordEntity,
    keywordLet,
    keywordPersistent,
    keywordVar,
    keywordOn,
    keywordIf,
    keywordAwait,
    unknown,
};

struct SourceLocation {
    std::size_t offset{};
    std::uint32_t line{1};
    std::uint32_t column{1};
};

struct Token {
    TokenKind kind{TokenKind::unknown};
    std::string_view text{};
    SourceLocation location{};
};

} // namespace sunrise::izanami::fate::lexer