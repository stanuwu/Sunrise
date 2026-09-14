#include "public_profiles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <optional>

#include "account_platform.h"

namespace sunrise::state::account::profiles {
namespace {
constexpr std::size_t kTokenSize = 32;
struct Entry {
    std::uint64_t primarySoid{};
    std::array<std::byte, kTokenSize> token{};
    std::unique_ptr<AccountState> profile;
    std::uint32_t generation{};
    std::uint32_t membershipGeneration{};
    network::PeerPublication peerPublication;
};
std::array<Entry, kAccountCapacity> g_entries;
std::size_t g_count = 1;
std::mutex g_lock;
std::atomic<std::uint64_t> g_localGeneration{1};
social::NativePresence g_localPresence;
std::atomic<std::uint32_t> g_publicGeneration{1};

bool present(AccountHandle handle) noexcept {
    return handle < g_count && g_entries[handle].primarySoid != 0;
}

bool remote(AccountHandle handle) noexcept {
    return handle != kLocalAccount && present(handle);
}

template <std::size_t N> bool valid_text(const std::array<char, N>& value) noexcept {
    bool ended = false;
    for (const auto byte : value) {
        if (byte == '\0') {
            ended = true;
        } else if (ended || byte < 32 || byte > 126) {
            return false;
        }
    }
    return ended;
}

bool overlaps(const AccountState& candidate, const Entry& other) noexcept {
    if (other.primarySoid == 0) {
        return false;
    }
    for (std::size_t i = 0; i < candidate.characterCount; ++i) {
        const auto soid = candidate.characters[i].soid;
        if (soid == other.primarySoid) {
            return true;
        }
        if (!other.profile) {
            continue;
        }
        for (std::size_t j = 0; j < other.profile->characterCount; ++j) {
            if (soid == other.profile->characters[j].soid) {
                return true;
            }
        }
    }
    if (other.profile) {
        for (std::size_t j = 0; j < other.profile->characterCount; ++j) {
            if (candidate.primarySoid == other.profile->characters[j].soid) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Re-derives one projected character's inventory row generations.
 *
 * The public record carries no private mutation journal, so the wire leaves every published row
 * at serial zero. The native character and inspection objects still publish a row generation per
 * row and one next-serial watermark, and the client orders a bucket's grid cells by them, so a
 * projected character needs its own dense, ordered generations rather than the owner's. Rows are
 * numbered in native equipment-slot order and then in inventory order, which is the order the
 * loadout resolver walks them in.
 * @param character Projected character whose rows carry no owner-supplied generation.
 */
void seed_row_generations(CharacterState& character) noexcept {
    std::uint32_t next = 0;
    for (std::optional<inventory::Item>& item : character.equipment.slots) {
        if (item.has_value()) {
            item->mutationSerial = static_cast<std::int32_t>(next++);
        }
    }
    const std::size_t count =
        (std::min)(character.inventory.count, character.inventory.values.size());
    for (std::size_t index = 0; index < count; ++index) {
        character.inventory.values[index].mutationSerial = static_cast<std::int32_t>(next++);
    }
    character.nextInventorySerial = next;
}
} // namespace

bool valid(const AccountState& profile) noexcept {
    const auto& presence = profile.presence;
    return platform::valid(presence.platformId)
           && platform::declared_account_soid(presence.platformId) == profile.primarySoid
           && (presence.flags & ~kPresenceFlagsMask) == 0 && valid_text(presence.displayName)
           && valid_text(presence.personaName) && valid_text(presence.nameCode)
           && account::valid_public(profile) && social::valid(presence.native)
           && (presence.native.characterSoid == 0
               || std::any_of(profile.characters.begin(),
                              profile.characters.begin()
                                  + static_cast<std::ptrdiff_t>(profile.characterCount),
                              [&](const CharacterState& character) {
                                  return character.soid == presence.native.characterSoid;
                              }));
}

void reset(std::uint64_t localSoid) noexcept {
    const std::lock_guard lock(g_lock);
    for (auto& entry : g_entries) {
        entry = {};
    }
    g_entries[kLocalAccount].primarySoid = localSoid;
    g_count = 1;
    g_localPresence = {};
    g_publicGeneration.fetch_add(1, std::memory_order_release);
    local_changed();
}

void local_changed() noexcept {
    g_localGeneration.fetch_add(1, std::memory_order_release);
}
std::uint64_t local_generation() noexcept {
    return g_localGeneration.load(std::memory_order_acquire);
}

bool publish_local_presence(AccountHandle owner, const social::NativePresence& value) noexcept {
    if (owner != kLocalAccount || !social::valid(value)) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    if (g_localPresence != value) {
        g_localPresence = value;
        g_entries[kLocalAccount].peerPublication.observe(value);
        local_changed();
    }
    return true;
}

social::NativePresence local_presence() noexcept {
    const std::lock_guard lock(g_lock);
    return g_localPresence;
}

social::NativePresence native_presence(AccountHandle owner) noexcept {
    const std::lock_guard lock(g_lock);
    if (owner == kLocalAccount) {
        return g_localPresence;
    }
    if (!remote(owner) || !g_entries[owner].profile) {
        return {};
    }
    const auto& profile = *g_entries[owner].profile;
    const auto& native = profile.presence.native;
    return native.published && native.characterSoid != 0
                   && native.characterSoid == account::selected_character_soid(profile)
               ? native
               : social::NativePresence{};
}

std::size_t peer_endpoints(AccountHandle owner,
                           std::span<network::PeerPublication::Endpoint> output) noexcept {
    const std::lock_guard lock(g_lock);
    if (!present(owner) || !g_entries[owner].profile
        || g_entries[owner].peerPublication.character
               != account::selected_character_soid(*g_entries[owner].profile)) {
        return 0;
    }
    return g_entries[owner].peerPublication.snapshot(output);
}

void clear_remote_presence(AccountHandle owner) noexcept {
    const std::lock_guard lock(g_lock);
    if (!remote(owner) || !g_entries[owner].profile) {
        return;
    }
    auto& entry = g_entries[owner];
    entry.peerPublication = {};
    if (entry.profile->presence.native == social::NativePresence{}) {
        return;
    }
    entry.profile->presence.native = {};
    ++entry.generation;
    g_publicGeneration.fetch_add(1, std::memory_order_release);
}

std::uint32_t public_generation() noexcept {
    return g_publicGeneration.load(std::memory_order_acquire);
}

void service_changed(AccountHandle owner) noexcept {
    const std::lock_guard lock(g_lock);
    if (owner == kLocalAccount) {
        local_changed();
        return;
    }
    if (!remote(owner) || !g_entries[owner].profile) {
        return;
    }
    ++g_entries[owner].generation;
    g_publicGeneration.fetch_add(1, std::memory_order_release);
}

std::size_t count() noexcept {
    const std::lock_guard lock(g_lock);
    return g_count;
}

bool release(AccountHandle handle, std::uint64_t expectedPrimarySoid) noexcept {
    const std::lock_guard lock(g_lock);
    if (!remote(handle) || g_entries[handle].primarySoid != expectedPrimarySoid) {
        return false;
    }
    auto& entry = g_entries[handle];
    entry.primarySoid = 0;
    entry.token = {};
    entry.profile.reset();
    entry.peerPublication = {};
    // Keep generations monotonic through reuse so retained subscription stamps cannot alias.
    if (++entry.generation == 0) {
        ++entry.generation;
    }
    if (++entry.membershipGeneration == 0) {
        ++entry.membershipGeneration;
    }
    g_publicGeneration.fetch_add(1, std::memory_order_release);
    return true;
}

bool reserve(std::uint64_t primarySoid,
             std::span<const std::byte> token,
             AccountHandle& handle,
             bool* attachedNow) noexcept {
    if (attachedNow) {
        *attachedNow = false;
    }
    if (primarySoid == 0 || token.size() != kTokenSize) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    if (primarySoid == g_entries[kLocalAccount].primarySoid) {
        return false;
    }
    if (g_entries[kLocalAccount].profile) {
        for (const auto& character : g_entries[kLocalAccount].profile->characters) {
            if (character.soid == primarySoid) {
                return false;
            }
        }
    }
    for (AccountHandle i = 1; i < g_count; ++i) {
        const auto& entry = g_entries[i];
        if (entry.primarySoid == 0) {
            continue;
        }
        const bool sameToken = std::equal(token.begin(), token.end(), entry.token.begin());
        if (entry.primarySoid == primarySoid) {
            if (!sameToken) {
                return false;
            }
            handle = i;
            return true;
        }
        if (sameToken) {
            return false;
        }
        if (entry.profile) {
            for (std::size_t j = 0; j < entry.profile->characterCount; ++j) {
                if (entry.profile->characters[j].soid == primarySoid) {
                    return false;
                }
            }
        }
    }
    handle = 1;
    while (handle < g_count && g_entries[handle].primarySoid != 0) {
        ++handle;
    }
    if (handle == g_entries.size()) {
        return false;
    }
    if (handle == g_count) {
        ++g_count;
    }
    auto& entry = g_entries[handle];
    entry.primarySoid = primarySoid;
    std::copy(token.begin(), token.end(), entry.token.begin());
    if (attachedNow) {
        *attachedNow = true;
    }
    return true;
}

bool find(std::uint64_t primarySoid, AccountHandle& handle) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    for (AccountHandle i = 0; i < g_count; ++i) {
        if (g_entries[i].primarySoid == primarySoid) {
            handle = i;
            return true;
        }
    }
    return false;
}

bool find_token(std::span<const std::byte> token, AccountHandle& handle) noexcept {
    if (token.size() != kTokenSize) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    for (AccountHandle i = 1; i < g_count; ++i) {
        if (g_entries[i].primarySoid != 0
            && std::equal(token.begin(), token.end(), g_entries[i].token.begin())) {
            handle = i;
            return true;
        }
    }
    return false;
}

bool find_platform(std::uint64_t platformId, AccountHandle& handle) noexcept {
    if (!platform::valid(platformId)) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    for (AccountHandle i = 1; i < g_count; ++i) {
        if (g_entries[i].profile && g_entries[i].profile->presence.platformId == platformId) {
            handle = i;
            return true;
        }
    }
    return false;
}

bool find_owner(std::uint64_t objectSoid, AccountHandle& handle) noexcept {
    if (objectSoid == 0) {
        return false;
    }
    const std::lock_guard lock(g_lock);
    for (AccountHandle i = 0; i < g_count; ++i) {
        const auto& entry = g_entries[i];
        if (entry.primarySoid == objectSoid) {
            handle = i;
            return true;
        }
        if (!entry.profile) {
            continue;
        }
        for (std::size_t j = 0; j < entry.profile->characterCount; ++j) {
            if (entry.profile->characters[j].soid == objectSoid) {
                handle = i;
                return true;
            }
        }
    }
    return false;
}

std::uint64_t primary_soid(AccountHandle handle) noexcept {
    const std::lock_guard lock(g_lock);
    return handle < g_count ? g_entries[handle].primarySoid : 0;
}

bool publish(AccountHandle handle, const AccountState& profile) noexcept {
    if (!valid(profile)) {
        return false;
    }
    auto candidate = std::unique_ptr<AccountState>(new (std::nothrow) AccountState{});
    if (!candidate) {
        return false;
    }
    candidate->primarySoid = profile.primarySoid;
    candidate->presence = profile.presence;
    candidate->characterCount = profile.characterCount;
    std::copy_n(profile.characters.begin(), profile.characterCount, candidate->characters.begin());
    for (auto& character : candidate->characters) {
        character.stacks = {};
        seed_row_generations(character);
    }
    const std::lock_guard lock(g_lock);
    if (!present(handle) || g_entries[handle].primarySoid != candidate->primarySoid) {
        return false;
    }
    for (AccountHandle i = 0; i < g_count; ++i) {
        if (i != handle && overlaps(*candidate, g_entries[i])) {
            return false;
        }
    }
    auto& entry = g_entries[handle];
    if (entry.profile && entry.profile->presence.platformId != candidate->presence.platformId) {
        return false;
    }
    if (!entry.profile || entry.profile->presence.displayName != candidate->presence.displayName
        || account::selected_character_soid(*entry.profile)
               != account::selected_character_soid(*candidate)) {
        if (++entry.membershipGeneration == 0) {
            ++entry.membershipGeneration;
        }
    }
    entry.peerPublication.observe(candidate->presence.native);
    entry.profile = std::move(candidate);
    if (++entry.generation == 0) {
        ++entry.generation;
    }
    g_publicGeneration.fetch_add(1, std::memory_order_release);
    return true;
}

bool snapshot(AccountHandle handle, AccountState& output) noexcept {
    const std::lock_guard lock(g_lock);
    if (!present(handle) || !g_entries[handle].profile) {
        output = {};
        return false;
    }
    output = *g_entries[handle].profile;
    return true;
}

std::uint32_t generation(AccountHandle handle) noexcept {
    const std::lock_guard lock(g_lock);
    return present(handle) ? g_entries[handle].generation : 0;
}

std::uint32_t membership_generation(AccountHandle handle) noexcept {
    const std::lock_guard lock(g_lock);
    return present(handle) ? g_entries[handle].membershipGeneration : 0;
}

std::uint64_t banner_character(AccountHandle handle) noexcept {
    const std::lock_guard lock(g_lock);
    return present(handle) && g_entries[handle].profile
               ? account::banner_character_soid(*g_entries[handle].profile)
               : 0;
}

std::uint64_t selected_character(AccountHandle handle) noexcept {
    const std::lock_guard lock(g_lock);
    return present(handle) && g_entries[handle].profile
               ? account::selected_character_soid(*g_entries[handle].profile)
               : 0;
}

bool selected_member_name(AccountHandle handle,
                          std::uint64_t characterSoid,
                          std::array<char, kDisplayNameCapacity>& name) noexcept {
    name = {};
    const std::lock_guard lock(g_lock);
    if (!present(handle) || !g_entries[handle].profile || !characterSoid
        || account::selected_character_soid(*g_entries[handle].profile) != characterSoid) {
        return false;
    }
    name = g_entries[handle].profile->presence.displayName;
    return true;
}

} // namespace sunrise::state::account::profiles
