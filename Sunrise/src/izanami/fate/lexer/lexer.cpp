#include "lexer.h"

#include <cctype>

namespace sunrise::izanami::fate::lexer {

namespace {

[[nodiscard]] bool is_identifier_start(char value) noexcept {
    return std::isalpha(static_cast<unsigned char>(value)) != 0 || value == '_';
}

[[nodiscard]] bool is_identifier_body(char value) noexcept {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

[[nodiscard]] bool is_digit(char value) noexcept {
    return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] TokenKind keyword_or_identifier(std::string_view text) noexcept {
    if (text == "entity") {
        return TokenKind::keywordEntity;
    }
    if (text == "let") {
        return TokenKind::keywordLet;
    }
    if (text == "persistent") {
        return TokenKind::keywordPersistent;
    }
    if (text == "var") {
        return TokenKind::keywordVar;
    }
    if (text == "on") {
        return TokenKind::keywordOn;
    }
    if (text == "if") {
        return TokenKind::keywordIf;
    }
    if (text == "await") {
        return TokenKind::keywordAwait;
    }
    return TokenKind::identifier;
}

} // namespace

Lexer::Lexer(std::string_view source) noexcept : source_(source) {}

/** Reads one Fate token and advances the lexer cursor. */
Token Lexer::next() noexcept {
    skip_whitespace();
    const SourceLocation location{.offset = offset_, .line = line_, .column = column_};
    if (at_end()) {
        return {.kind = TokenKind::endOfFile, .location = location};
    }

    const std::size_t start = offset_;
    const char first = advance();
    if (is_identifier_start(first)) {
        while (!at_end() && is_identifier_body(peek())) {
            (void)advance();
        }
        const std::string_view text = source_.substr(start, offset_ - start);
        return {.kind = keyword_or_identifier(text), .text = text, .location = location};
    }
    if (is_digit(first)) {
        while (!at_end() && is_digit(peek())) {
            (void)advance();
        }
        const std::string_view text = source_.substr(start, offset_ - start);
        return {.kind = TokenKind::number, .text = text, .location = location};
    }
    if (first == '"') {
        while (!at_end() && peek() != '"') {
            (void)advance();
        }
        if (!at_end()) {
            (void)advance();
        }
        return {.kind = TokenKind::stringLiteral,
                .text = source_.substr(start, offset_ - start),
                .location = location};
    }

    TokenKind kind = TokenKind::unknown;
    switch (first) {
    case '{':
        kind = TokenKind::leftBrace;
        break;
    case '}':
        kind = TokenKind::rightBrace;
        break;
    case '(':
        kind = TokenKind::leftParen;
        break;
    case ')':
        kind = TokenKind::rightParen;
        break;
    case ':':
        kind = TokenKind::colon;
        break;
    case ',':
        kind = TokenKind::comma;
        break;
    case '.':
        kind = TokenKind::dot;
        break;
    case '=':
        kind = TokenKind::equal;
        break;
    default:
        break;
    }
    return {.kind = kind, .text = source_.substr(start, 1), .location = location};
}

bool Lexer::at_end() const noexcept {
    return offset_ >= source_.size();
}

char Lexer::peek() const noexcept {
    return at_end() ? '\0' : source_[offset_];
}

char Lexer::advance() noexcept {
    const char value = source_[offset_++];
    if (value == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return value;
}

void Lexer::skip_whitespace() noexcept {
    while (!at_end()) {
        const char value = peek();
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            (void)advance();
            continue;
        }
        break;
    }
}

} // namespace sunrise::izanami::fate::lexer