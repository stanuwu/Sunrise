#include "log_snapshot_view.h"

#include <algorithm>

namespace sunrise::core::log::view {
namespace {

/** @param value ASCII byte to fold. @return Lowercase ASCII byte or the unchanged value. */
[[nodiscard]] char fold_ascii(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

/**
 * Tests one formatted event against the bounded ASCII text query.
 * @param text Formatted event text.
 * @param query Case-insensitive query.
 * @return True when query is empty or occurs in text.
 */
[[nodiscard]] bool contains_text(std::string_view text, std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    const auto match = std::search(
        text.begin(), text.end(), query.begin(), query.end(), [](char left, char right) noexcept {
            return fold_ascii(left) == fold_ascii(right);
        });
    return match != text.end();
}

/**
 * Tests one retained event against every active filter field.
 * @param entry Value-owned retained event.
 * @param filter Channel, level, and text constraints.
 * @return True when every active constraint matches.
 */
[[nodiscard]] bool matches(const snapshot::Entry& entry, const Filter& filter) noexcept {
    if (filter.channel.has_value() && entry.channel() != *filter.channel) {
        return false;
    }
    if (filter.level.has_value() && entry.level() != *filter.level) {
        return false;
    }
    const std::size_t textLength = (std::min)(filter.textLength, filter.text.size());
    return contains_text(entry.text(), std::string_view(filter.text.data(), textLength));
}

} // namespace

/** @return Matching entry pointers borrowing storage from the caller-owned source snapshot. */
std::span<const snapshot::Entry* const> Result::entries() const noexcept {
    return {entries_.data(), count_};
}

/** @return Count retained by the source logger snapshot. */
std::size_t Result::source_count() const noexcept {
    return sourceCount_;
}

/** @return Count of older entries replaced before the source snapshot. */
std::uint64_t Result::overwritten_count() const noexcept {
    return overwrittenCount_;
}

/** Replaces the bounded text query without changing it on overflow. */
bool set_text(Filter& filter, std::string_view text) noexcept {
    if (text.size() > filter.text.size()) {
        return false;
    }
    filter.text = {};
    std::copy(text.begin(), text.end(), filter.text.begin());
    filter.textLength = text.size();
    return true;
}

/**
 * Applies every active filter to one value-owned logger snapshot.
 * @param source Chronological logger snapshot.
 * @param filter Optional exact channel, exact level, and ASCII text query.
 * @return Bounded selection borrowing entries from source for its unchanged lifetime.
 */
Result select(const snapshot::Snapshot& source, const Filter& filter) noexcept {
    Result result;
    result.sourceCount_ = source.entries().size();
    result.overwrittenCount_ = source.overwritten_count();
    for (const snapshot::Entry& entry : source.entries()) {
        if (matches(entry, filter)) {
            result.entries_[result.count_] = &entry;
            ++result.count_;
        }
    }
    return result;
}

} // namespace sunrise::core::log::view
