#pragma once

#include <cstdint>

namespace sunrise::state::activity {

/** The exact native member whose activity connection is currently being serviced. */
struct MemberContext final {
    std::uint64_t sessionId{};
    std::uint64_t memberKey{};
};

/** Nested service calls restore the original activity owner on every exit. */
class ScopedMemberContext final {
public:
    explicit ScopedMemberContext(std::uint64_t sessionId, std::uint64_t memberKey) noexcept;
    ~ScopedMemberContext() noexcept;
    ScopedMemberContext(const ScopedMemberContext&) = delete;
    ScopedMemberContext& operator=(const ScopedMemberContext&) = delete;

private:
    MemberContext previous_{};
};

/** Returns this thread's current scoped activity owner, or the empty default outside a scope. */
[[nodiscard]] MemberContext member_context() noexcept;
} // namespace sunrise::state::activity
