#include "parser.h"

namespace sunrise::izanami::fate::parser {

Parser::Parser(std::span<const lexer::Token> tokens) noexcept : tokens_(tokens) {}

/** Parses top-level Fate declarations that are stable enough for early tooling. */
ParseResult Parser::parse_module() {
    ParseResult result;
    while (!at_end()) {
        const lexer::Token& token = advance();
        if (token.kind == lexer::TokenKind::keywordEntity && !at_end()) {
            const lexer::Token& name = advance();
            if (name.kind == lexer::TokenKind::identifier) {
                result.module.entities.push_back({.name = name});
            } else {
                result.ok = false;
            }
        }
    }
    return result;
}

bool Parser::at_end() const noexcept {
    return index_ >= tokens_.size() || peek().kind == lexer::TokenKind::endOfFile;
}

const lexer::Token& Parser::peek() const noexcept {
    return tokens_[index_];
}

const lexer::Token& Parser::advance() noexcept {
    return tokens_[index_++];
}

} // namespace sunrise::izanami::fate::parser