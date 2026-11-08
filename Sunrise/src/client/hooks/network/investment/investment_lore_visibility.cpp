/**
 * Clears the twenty-four literal-true presentation gates on the shipped lore books.
 * A literal gate reads no unlock slot, so no authored unlock policy can clear it.
 */

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808075.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../content/handles/handle_resolver.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game/content.h"
#include "internal.h"
#include "lore_visibility_patch.h"
#include "socket_menu_native_array.h"

namespace sunrise::client::hooks::network::investment {
namespace {

namespace tables = middleware::content::packages::tables;
namespace generated = middleware::content::packages::schema::generated;

/** Installed tags of the record table and the presentation node table, in that order. */
constexpr std::array<std::uint32_t, 2> kTableTags{0x81319339U, 0x8131933FU};
/** Row array field of each of those tables, in the same order. */
constexpr std::array<std::size_t, 2> kTableArrayFields{offsetof(generated::C80807567, table),
                                                       offsetof(generated::C80803041, table)};
/** Rows each of those tables holds in this build. A different count is a different build. */
constexpr std::array<std::size_t, 2> kTableRowCounts{2242, 924};
/** Row size of each of those tables, in the same order. */
constexpr std::array<std::size_t, 2> kTableRowStrides{tables::kRecordRowStride,
                                                      tables::kNodeRowStride};
/** Row class of each of those tables, in the same order. */
constexpr std::array<std::uint32_t, 2> kTableRowClasses{generated::RecordRow::kClass,
                                                        generated::PresentationNodeRow::kClass};
/** Expression fields swept on a record row. */
constexpr std::array<std::size_t, 2> kRecordExpressionFields{
    tables::kRecordCategoryExpressionField, tables::kRecordAlternateExpressionField};
/** Expression fields swept on a node row. */
constexpr std::array<std::size_t, 2> kNodeExpressionFields{tables::kNodeExpressionFieldAlternate,
                                                           tables::kNodeExpressionFieldPrimary};
/** Table index of the record rows, which is the only table holding a patched gate. */
constexpr std::size_t kRecordTable = 0;
/** Both tables carry the row's authored definition hash at this offset. */
constexpr std::size_t kRowHashOffset = tables::kRecordHashOffset;
/** One array field: how many rows it holds and where they sit. */
using Descriptor = native_array::ArrayDescriptor;

/** @return The field's row count, read as the unsigned word the native reader uses. */
[[nodiscard]] std::uint64_t count_of(const Descriptor& desc) noexcept {
    return static_cast<std::uint64_t>(desc.count);
}

/** One replaced instruction and both values it takes. */
struct Patch {
    std::uintptr_t address;
    lore::Instruction before;
    lore::Instruction after;
};

std::array<Patch, lore::kTargets.size()> g_patches{};
std::size_t g_count{};
SRWLOCK g_lock = SRWLOCK_INIT;

template <class T> bool read(std::uintptr_t address, T& value) noexcept {
    return memory::read_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&value, 1)));
}

/**
 * Follows one array field to its first row.
 * @param address Address of the field itself.
 * @param maximumCount Largest row count accepted.
 * @param stride Row size; the last row must be readable.
 * @param elementClass Class the array header must name.
 * @param data Receives the address of the first row.
 * @return False when the header does not repeat the count or names another class.
 */
bool data_at(std::uintptr_t address,
             std::size_t maximumCount,
             std::size_t stride,
             std::uint32_t elementClass,
             std::uintptr_t& data) noexcept {
    native_array::ArrayView view{};
    if (!native_array::resolve_array(address, maximumCount, stride, elementClass, view)) {
        return false;
    }
    data = view.data;
    return true;
}

/**
 * Replaces one instruction, or puts the original back.
 * @param patch Recorded address and both values.
 * @param restore True to write the original value.
 * @return False unless the expected value was there and the new one reads back.
 */
bool write(const Patch& patch, bool restore) noexcept {
    const auto expected = restore ? patch.after : patch.before;
    const auto desired = restore ? patch.before : patch.after;
    lore::Instruction current{};
    if (!read(patch.address, current) || current != expected) {
        return false;
    }
    void* destination = native_array::image_pointer(patch.address);
    DWORD previous{};
    if (!VirtualProtect(destination, sizeof desired, PAGE_READWRITE, &previous)) {
        return false;
    }
    SIZE_T written{};
    const bool copied =
        WriteProcessMemory(GetCurrentProcess(), destination, &desired, sizeof desired, &written)
        && written == sizeof desired;
    DWORD ignored{};
    const bool protectedAgain =
        VirtualProtect(destination, sizeof desired, previous, &ignored) != FALSE;
    return copied && protectedAgain && read(patch.address, current) && current == desired;
}

/** Puts every recorded instruction back. Ownership is kept when any one of them refuses. */
bool rollback() noexcept {
    bool ok = true;
    for (std::size_t i = g_count; i > 0; --i) {
        lore::Instruction current{};
        if (!read(g_patches[i - 1].address, current)) {
            ok = false;
            continue;
        }
        if (current == g_patches[i - 1].before) {
            continue;
        }
        if (!write(g_patches[i - 1], true)) {
            ok = false;
        }
    }
    if (ok) {
        g_count = 0;
    }
    return ok;
}

/**
 * Resolves both tables and stages one patch per target.
 * @param staged Receives an address and both instruction values for every target.
 * @return False when any table, row, hash or expression is not the one this build expects.
 */
bool prepare(std::array<Patch, lore::kTargets.size()>& staged) noexcept {
    content::handles::Source source{};
    source.tablesSlot =
        reinterpret_cast<std::uintptr_t>(targets::game::content::get().contentHandleTablesSlot);
    source.read = &memory::read_current_process;
    std::array<std::uintptr_t, kTableTags.size()> rows{};
    for (std::size_t i = 0; i < kTableTags.size(); ++i) {
        std::uintptr_t table{};
        Descriptor desc{};
        if (!content::handles::resolve(source, kTableTags[i], table)
            || !read(table + kTableArrayFields[i], desc) || count_of(desc) != kTableRowCounts[i]
            || !data_at(table + kTableArrayFields[i],
                        kTableRowCounts[i],
                        kTableRowStrides[i],
                        kTableRowClasses[i],
                        rows[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < staged.size(); ++i) {
        const auto& target = lore::kTargets[i];
        const auto row = rows[kRecordTable] + target.row * kTableRowStrides[kRecordTable];
        std::uint32_t hash{};
        Descriptor desc{};
        std::uintptr_t data{};
        std::array<lore::Instruction, lore::kMaximumInstructions> code{};
        if (!read(row + kRowHashOffset, hash) || hash != target.hash
            || !read(row + target.field, desc) || count_of(desc) == 0
            || count_of(desc) > code.size()
            || !data_at(row + target.field,
                        code.size(),
                        tables::kUnlockInstructionStride,
                        tables::kInvestmentExpressionRowClass,
                        data)
            || !memory::read_current_process(nullptr,
                                             data,
                                             std::as_writable_bytes(std::span(code).first(
                                                 static_cast<std::size_t>(count_of(desc)))))
            || !lore::replacement(std::span(code).first(static_cast<std::size_t>(count_of(desc))),
                                  staged[i].after)) {
            return false;
        }
        staged[i].address = data;
        staged[i].before = code[0];
    }
    // Each edited instruction must be owned by exactly one presentation condition. Never mutate
    // a constant shared with another record, even if that record is not in this repair's list.
    std::array<unsigned, lore::kTargets.size()> references{};
    for (std::size_t kind = 0; kind < kTableTags.size(); ++kind) {
        for (std::size_t row = 0; row < kTableRowCounts[kind]; ++row) {
            const auto& fields =
                kind == kRecordTable ? kRecordExpressionFields : kNodeExpressionFields;
            for (const std::size_t field : fields) {
                const auto at = rows[kind] + row * kTableRowStrides[kind] + field;
                Descriptor desc{};
                std::uintptr_t data{};
                if (!read(at, desc)) {
                    return false;
                }
                if (count_of(desc) == 0) {
                    continue;
                }
                if (count_of(desc) > static_cast<std::uint64_t>(tables::kNodeExpressionCapacity)
                    || !data_at(at,
                                tables::kNodeExpressionCapacity,
                                tables::kUnlockInstructionStride,
                                tables::kInvestmentExpressionRowClass,
                                data)) {
                    return false;
                }
                const auto span = count_of(desc) * tables::kUnlockInstructionStride;
                for (std::size_t i = 0; i < staged.size(); ++i) {
                    if (staged[i].address >= data && staged[i].address - data < span) {
                        ++references[i];
                    }
                }
            }
        }
    }
    for (const unsigned count : references) {
        if (count != 1) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears every literal lore gate, or leaves all twenty-four exactly as they were. */
void apply_lore_visibility() noexcept {
    if (!core::settings::get().client.revealLoreBooks) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_count != 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    std::array<Patch, lore::kTargets.size()> staged{};
    bool ok = prepare(staged);
    if (ok) {
        for (const auto& patch : staged) {
            g_patches[g_count++] = patch;
            if (!write(patch, false)) {
                ok = false;
                break;
            }
        }
    }
    const bool restored = ok || rollback();
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=lore_visibility result=%s conditions=%zu",
                                      ok ? "applied" : (restored ? "refused" : "rollback_failed"),
                                      g_count);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         ok ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Puts every value the patch replaced back. */
void restore_lore_visibility() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!rollback()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=lore_visibility result=restore_failed");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::client::hooks::network::investment
