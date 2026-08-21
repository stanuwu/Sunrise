/* Included once by state_account_identity_runtime.cpp. */

#include <string_view>

#include "../../core/settings/character_persistence.h"
#include "../../core/settings/settings.h"
#include "../../../resources/resource.h"
#include "character_creation.h"

namespace sunrise::state {
namespace {

struct FactoryTemplateCache final {
    AccountState account{};
    bool ready{};
};

/** Module-local address used only to resolve Sunrise's own embedded resources. */
const std::byte g_factoryResourceAnchor{};

[[nodiscard]] FactoryTemplateCache load_factory_templates() noexcept {
    FactoryTemplateCache cache{};
    HMODULE module = nullptr;
    const auto moduleAddress = reinterpret_cast<LPCWSTR>(&g_factoryResourceAnchor);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           moduleAddress,
                           &module)
        == FALSE) {
        return cache;
    }
    const HRSRC resource =
        FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_SETTINGS), RT_RCDATA);
    if (resource == nullptr) {
        return cache;
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* bytes = loaded == nullptr ? nullptr : LockResource(loaded);
    if (bytes == nullptr || size == 0) {
        return cache;
    }

    std::string_view json(static_cast<const char*>(bytes), static_cast<std::size_t>(size));
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEFU
        && static_cast<unsigned char>(json[1]) == 0xBBU
        && static_cast<unsigned char>(json[2]) == 0xBFU) {
        json.remove_prefix(3);
    }
    core::settings::Settings parsed{};
    if (!core::settings::parse(json, parsed) || !account::valid_authored(parsed.initialAccount)) {
        return cache;
    }
    cache.account = parsed.initialAccount;
    cache.ready = true;
    return cache;
}

[[nodiscard]] const FactoryTemplateCache& factory_templates() noexcept {
    static const FactoryTemplateCache cache = load_factory_templates();
    return cache;
}

[[nodiscard]] bool template_for_class(CharacterClass characterClass,
                                      CharacterState& output) noexcept {
    output = {};
    const FactoryTemplateCache& cache = factory_templates();
    if (!cache.ready) {
        return false;
    }
    bool found = false;
    for (std::size_t index = 0; index < cache.account.characterCount; ++index) {
        const CharacterState& candidate = cache.account.characters[index];
        if (candidate.characterClass != characterClass) {
            continue;
        }
        if (found) {
            return false;
        }
        output = candidate;
        found = true;
    }
    return found;
}

[[nodiscard]] bool next_character_soid(const AccountState& accountState,
                                       std::uint64_t& output) noexcept {
    output = 0;
    if (accountState.primarySoid == 0) {
        return false;
    }
    for (std::uint64_t offset = 1; offset <= kCharacterCapacity + 1U; ++offset) {
        if (accountState.primarySoid
            > (std::numeric_limits<std::uint64_t>::max)() - offset) {
            return false;
        }
        const std::uint64_t candidate = accountState.primarySoid + offset;
        if (!runtime::detail::account_owns_soid(accountState, candidate)) {
            output = candidate;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool remap_created_items(AccountState& accountState,
                                       std::size_t characterIndex) noexcept {
    if (characterIndex >= accountState.characterCount) {
        return false;
    }
    CharacterState& character = accountState.characters[characterIndex];
    std::uint32_t nextSerial = 0;
    const auto assign_fresh = [&](account::inventory::Item& item) noexcept {
        std::uint64_t fresh = 0;
        if (!runtime::detail::next_item_instance_soid(accountState, fresh)
            || nextSerial > static_cast<std::uint32_t>(
                                (std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        item.instanceSoid = fresh;
        item.mutationSerial = static_cast<std::int32_t>(nextSerial++);
        return true;
    };
    for (auto& item : character.equipment.slots) {
        if (item.has_value() && !assign_fresh(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (!assign_fresh(character.inventory.values[index])) {
            return false;
        }
    }
    character.nextInventorySerial = nextSerial;
    return true;
}

[[nodiscard]] bool same_native_creation(const NativeCharacterCreation& left,
                                        const NativeCharacterCreation& right) noexcept {
    return left.race == right.race && left.gender == right.gender
           && left.characterClass == right.characterClass
           && left.presentationHeader == right.presentationHeader
           && left.creationHeader == right.creationHeader && left.creationTail == right.creationTail
           && left.creatorTrailer == right.creatorTrailer;
}

[[nodiscard]] bool same_pending_creation(const PendingCharacterCreation& left,
                                         const PendingCharacterCreation& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.beforeCharacterCount == right.beforeCharacterCount
           && left.characterIndex == right.characterIndex
           && left.selectCreated == right.selectCreated
           && same_native_creation(left.creation, right.creation)
           && runtime::detail::same_character(left.createdCharacter, right.createdCharacter);
}

[[nodiscard]] CharacterCreationResult
stage_character_creation(const AccountState& before,
                         const NativeCharacterCreation& creation,
                         PendingCharacterCreation& mutation,
                         AccountState& after) noexcept {
    mutation = {};
    after = {};
    if (!account::valid(before) || before.primarySoid == 0
        || creation.race > CharacterRace::exo || creation.gender > CharacterGender::female
        || creation.characterClass > CharacterClass::warlock || creation.creatorTrailer > 0x1FU) {
        return CharacterCreationResult::invalid;
    }
    if (before.characterCount >= before.characters.size()) {
        return CharacterCreationResult::full;
    }

    CharacterState templateCharacter{};
    if (!template_for_class(creation.characterClass, templateCharacter)) {
        return CharacterCreationResult::missingTemplate;
    }
    std::uint64_t characterSoid = 0;
    if (!next_character_soid(before, characterSoid)) {
        return CharacterCreationResult::invalid;
    }

    after = before;
    const std::size_t characterIndex = after.characterCount;
    const bool selectCreated = after.characterCount != 0;
    if (selectCreated) {
        for (std::size_t index = 0; index < after.characterCount; ++index) {
            after.characters[index].selected = false;
        }
    }

    CharacterState created = templateCharacter;
    created.soid = characterSoid;
    created.selected = selectCreated;
    created.race = creation.race;
    created.gender = creation.gender;
    created.characterClass = creation.characterClass;
    created.presentationHeader = creation.presentationHeader;
    created.creationHeader = creation.creationHeader;
    created.creationTail = creation.creationTail;
    created.creatorTrailer = creation.creatorTrailer;
    after.characters[characterIndex] = created;
    ++after.characterCount;

    if (!remap_created_items(after, characterIndex)) {
        return CharacterCreationResult::invalid;
    }
    created = after.characters[characterIndex];

    middleware::datagen::family4::loadout::ResolvedLoadout loadout{};
    if (!account::valid(after)
        || !middleware::datagen::family4::loadout::resolve(after, characterIndex, loadout)) {
        return CharacterCreationResult::invalid;
    }

    mutation.creation = creation;
    mutation.createdCharacter = created;
    mutation.accountSoid = before.primarySoid;
    mutation.characterSoid = created.soid;
    mutation.beforeCharacterCount = before.characterCount;
    mutation.characterIndex = characterIndex;
    mutation.selectCreated = selectCreated;
    mutation.prepared = true;
    return CharacterCreationResult::ok;
}

} // namespace

CharacterCreationResult prepare_character_creation(const NativeCharacterCreation& creation,
                                                    PendingCharacterCreation& mutation) noexcept {
    AccountState ignored{};
    return stage_character_creation(account_snapshot(), creation, mutation, ignored);
}

bool preview_character_creation(const PendingCharacterCreation& mutation,
                                AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared) {
        return false;
    }
    PendingCharacterCreation canonical{};
    const CharacterCreationResult result =
        stage_character_creation(account_snapshot(), mutation.creation, canonical, after);
    return result == CharacterCreationResult::ok && same_pending_creation(canonical, mutation);
}

bool commit_character_creation(PendingCharacterCreation& mutation) noexcept {
    const PendingCharacterCreation prepared = mutation;
    mutation = {};
    if (!prepared.prepared) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    PendingCharacterCreation canonical{};
    AccountState after{};
    const AccountState before = runtime::storage::g_state.account;
    const CharacterCreationResult result =
        stage_character_creation(before, prepared.creation, canonical, after);
    bool committed = result == CharacterCreationResult::ok
                     && same_pending_creation(canonical, prepared) && account::valid(after);

    // Disk and runtime State cross the same boundary. Persistence also reconciles an older stale
    // authored roster against this exact before/after pair, so a duplicate row can never create a
    // character that exists only until restart.
    if (committed) {
        committed = core::settings::persistence::store_character(
            before, after, prepared.characterSoid);
    }
    if (committed) {
        runtime::storage::g_state.account = after;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

const char* character_creation_result_name(CharacterCreationResult result) noexcept {
    switch (result) {
    case CharacterCreationResult::ok:
        return "ok";
    case CharacterCreationResult::full:
        return "full";
    case CharacterCreationResult::missingTemplate:
        return "missing_template";
    case CharacterCreationResult::invalid:
    default:
        return "invalid";
    }
}

} // namespace sunrise::state
