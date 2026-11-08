#include "scriptable_catalog_inline_names.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/reader/container_framing.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../state/build_data/scriptables/inline_name_evidence.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace catalog = state::build_data::scriptables;
namespace evidence = state::build_data::scriptables::inline_name_evidence;

using InlineString = middleware::content::packages::schema::generated::SInlineString;
/** An inline string is its class word, then its record, then the text. */
constexpr std::size_t kInlineStringLengthOffset =
    middleware::content::packages::reader::layout::kClassWordSize
    + offsetof(InlineString, byteLength);
constexpr std::size_t kInlineStringHeaderBytes =
    middleware::content::packages::reader::layout::kClassWordSize + sizeof(InlineString);

/** Reads one trivially copied value from a bounded package blob. */
template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    output = {};
    if (offset > blob.size() || sizeof output > blob.size() - offset) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

/** @return The exact checked bytes owned by one raw evidence row. */
[[nodiscard]] bool candidate_bytes(const catalog::Snapshot& snapshot,
                                   const catalog::InlineNameCandidate& row,
                                   std::span<const std::byte>& output) noexcept {
    const std::size_t first = row.firstByte;
    const std::size_t count = row.byteCount;
    if (count == 0 || count > catalog::kInlineNameMaximumBytes
        || first > snapshot.inlineNameBytes.size()
        || count > snapshot.inlineNameBytes.size() - first) {
        output = {};
        return false;
    }
    output = std::span(snapshot.inlineNameBytes).subspan(first, count);
    return std::find(output.begin(), output.end(), std::byte{}) == output.end()
           && evidence::valid_utf8(output) && evidence::hash(output) == row.hash;
}

/** Appends one encountered tuple for one linear sort and dedupe after all package reads. */
[[nodiscard]] bool
append_candidate(catalog::Snapshot& output, std::uint32_t hash, std::span<const std::byte> bytes) {
    // Candidate rows and byte offsets are published as u32.
    constexpr std::size_t kMaximumOffset = (std::numeric_limits<std::uint32_t>::max)();
    if (output.inlineNameCandidates.size() >= kMaximumOffset
        || output.inlineNameBytes.size() > kMaximumOffset
        || bytes.size() > kMaximumOffset - output.inlineNameBytes.size()) {
        return false;
    }
    const std::size_t first = output.inlineNameBytes.size();
    output.inlineNameBytes.insert(output.inlineNameBytes.end(), bytes.begin(), bytes.end());
    try {
        output.inlineNameCandidates.push_back(
            {hash, static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bytes.size())});
    } catch (...) {
        output.inlineNameBytes.resize(first);
        throw;
    }
    return true;
}

/** Adds one visited candidate to a snapshot-owned evidence bank. */
[[nodiscard]] bool
collect_candidate(void* context, std::uint32_t hash, std::span<const std::byte> bytes) noexcept {
    if (context == nullptr) {
        return false;
    }
    try {
        return append_candidate(*static_cast<catalog::Snapshot*>(context), hash, bytes);
    } catch (...) {
        return false;
    }
}

} // namespace

/** Visits every bounded valid inline UTF-8 record in one reached package blob. */
bool visit_inline_names(std::span<const std::byte> blob,
                        InlineNameVisitor visitor,
                        void* context) noexcept {
    if (visitor == nullptr || blob.size() < kInlineStringHeaderBytes) {
        return visitor != nullptr;
    }
    for (std::size_t offset = 0; offset <= blob.size() - kInlineStringHeaderBytes; offset += 4) {
        std::uint32_t classId = 0;
        std::uint64_t encodedLength = 0;
        if (!read_value(blob, offset, classId) || classId != InlineString::kClass
            || !read_value(blob, offset + kInlineStringLengthOffset, encodedLength)
            || encodedLength == 0
            || encodedLength > blob.size() - offset - kInlineStringHeaderBytes) {
            continue;
        }
        const auto encoded = blob.subspan(offset + kInlineStringHeaderBytes,
                                          static_cast<std::size_t>(encodedLength));
        const auto terminator = std::find(encoded.begin(), encoded.end(), std::byte{});
        const auto bytes = encoded.first(static_cast<std::size_t>(terminator - encoded.begin()));
        if (bytes.empty() || bytes.size() > catalog::kInlineNameMaximumBytes
            || !evidence::valid_utf8(bytes)) {
            continue;
        }
        const std::uint32_t hash = evidence::hash(bytes);
        if (!visitor(context, hash, bytes)) {
            return false;
        }
    }
    return true;
}

/** Adds every valid inline UTF-8 record in one blob to the snapshot-local evidence bank. */
bool collect_inline_name_evidence(catalog::Snapshot& output,
                                  std::span<const std::byte> blob) noexcept {
    return visit_inline_names(blob, &collect_candidate, &output);
}

/** Sorts and repacks the complete snapshot-local evidence bank by hash and exact bytes. */
bool canonicalize_inline_name_evidence(catalog::Snapshot& output) noexcept {
    try {
        std::vector<std::size_t> order(output.inlineNameCandidates.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        for (const catalog::InlineNameCandidate& row : output.inlineNameCandidates) {
            std::span<const std::byte> bytes{};
            if (!candidate_bytes(output, row, bytes)) {
                return false;
            }
        }
        const auto bytes_at = [&output](std::size_t index) noexcept {
            const catalog::InlineNameCandidate& row = output.inlineNameCandidates[index];
            return std::span(output.inlineNameBytes).subspan(row.firstByte, row.byteCount);
        };
        std::sort(order.begin(),
                  order.end(),
                  [&output, &bytes_at](std::size_t left, std::size_t right) noexcept {
                      const catalog::InlineNameCandidate& leftRow =
                          output.inlineNameCandidates[left];
                      const catalog::InlineNameCandidate& rightRow =
                          output.inlineNameCandidates[right];
                      if (leftRow.hash != rightRow.hash) {
                          return leftRow.hash < rightRow.hash;
                      }
                      const auto leftBytes = bytes_at(left);
                      const auto rightBytes = bytes_at(right);
                      return std::lexicographical_compare(
                          leftBytes.begin(), leftBytes.end(), rightBytes.begin(), rightBytes.end());
                  });

        std::vector<catalog::InlineNameCandidate> rows{};
        std::vector<std::byte> bank{};
        rows.reserve(order.size());
        bank.reserve(output.inlineNameBytes.size());
        for (const std::size_t index : order) {
            const catalog::InlineNameCandidate& source = output.inlineNameCandidates[index];
            const auto bytes = bytes_at(index);
            if (!rows.empty() && rows.back().hash == source.hash) {
                const auto prior =
                    std::span(bank).subspan(rows.back().firstByte, rows.back().byteCount);
                if (std::equal(prior.begin(), prior.end(), bytes.begin(), bytes.end())) {
                    continue;
                }
            }
            if (bank.size() > (std::numeric_limits<std::uint32_t>::max)()
                || bytes.size() > (std::numeric_limits<std::uint32_t>::max)() - bank.size()) {
                return false;
            }
            const std::uint32_t first = static_cast<std::uint32_t>(bank.size());
            bank.insert(bank.end(), bytes.begin(), bytes.end());
            rows.push_back({source.hash, first, static_cast<std::uint32_t>(bytes.size())});
        }
        output.inlineNameCandidates = std::move(rows);
        output.inlineNameBytes = std::move(bank);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
