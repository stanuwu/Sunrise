#include "member_context.h"

namespace sunrise::state::activity {
namespace {
thread_local MemberContext current;
}

ScopedMemberContext::ScopedMemberContext(std::uint64_t sessionId, std::uint64_t memberKey) noexcept
    : previous_(current) {
    current = {sessionId, memberKey};
}
ScopedMemberContext::~ScopedMemberContext() noexcept {
    current = previous_;
}
MemberContext member_context() noexcept {
    return current;
}
} // namespace sunrise::state::activity
