#include "activity_context_slot_directive.h"

namespace sunrise::client::hooks::activity_context_slot {
namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::string_view kLineComment = "--";
constexpr std::string_view kDirective = "--!";
constexpr std::string_view kKey = "sunrise.activity_context_slot";

[[nodiscard]] constexpr bool blank(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && blank(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && blank(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

/** @return True for `--[[` and `--[==[`, whose body lines need not start with `--`. */
[[nodiscard]] constexpr bool opens_block_comment(std::string_view line) noexcept {
    if (!line.starts_with("--[")) {
        return false;
    }
    line.remove_prefix(3);
    while (!line.empty() && line.front() == '=') {
        line.remove_prefix(1);
    }
    return !line.empty() && line.front() == '[';
}

/** Parses the text after `--!`. Lines for other directives return `none`. */
[[nodiscard]] constexpr Request parse_directive(std::string_view body) noexcept {
    body = trim(body);
    if (!body.starts_with(kKey)) {
        return Request::none;
    }
    body.remove_prefix(kKey.size());
    if (!body.empty() && !blank(body.front()) && body.front() != '=') {
        // A longer key that only shares this prefix.
        return Request::none;
    }
    body = trim(body);
    if (body.empty() || body.front() != '=') {
        return Request::invalid;
    }
    std::string_view value = trim(body.substr(1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    if (value == "swapped") {
        return Request::swapped;
    }
    if (value == "native") {
        return Request::native;
    }
    return Request::invalid;
}

} // namespace

Request read_directive(std::string_view header, bool wholeFile) noexcept {
    if (header.starts_with(kByteOrderMark)) {
        header.remove_prefix(kByteOrderMark.size());
    }
    Request found = Request::none;
    while (!header.empty()) {
        const std::size_t end = header.find('\n');
        if (end == std::string_view::npos && !wholeFile) {
            break;
        }
        const std::string_view line =
            trim(end == std::string_view::npos ? header : header.substr(0, end));
        header.remove_prefix(end == std::string_view::npos ? header.size() : end + 1);
        if (line.empty()) {
            continue;
        }
        if (!line.starts_with(kLineComment) || opens_block_comment(line)) {
            break;
        }
        if (!line.starts_with(kDirective)) {
            continue;
        }
        const Request request = parse_directive(line.substr(kDirective.size()));
        if (request == Request::none) {
            continue;
        }
        if (request == Request::invalid || found != Request::none) {
            return Request::invalid;
        }
        found = request;
    }
    return found;
}

const char* request_name(Request value) noexcept {
    switch (value) {
    case Request::none:
        return "none";
    case Request::native:
        return "native";
    case Request::swapped:
        return "swapped";
    case Request::invalid:
        return "invalid";
    }
    return "unknown";
}

} // namespace sunrise::client::hooks::activity_context_slot
