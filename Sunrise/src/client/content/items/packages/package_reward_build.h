#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/rewards/definition.h"

namespace sunrise::client::content::items::packages {

struct SlotMaps;

/** A full reward bank settles the domain; failed storage is retried on a later pass. */
enum class BankFailure : std::uint8_t { none, full, storage };

/** Resolves expression references and unlock slots once for a package pass. */
class RewardConditions {
public:
    /** Reads the root's flag, value, expression and class tables; false when one is missing. */
    [[nodiscard]] bool load(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept;
    /** @return True when the last load() succeeded. */
    [[nodiscard]] bool loaded() const noexcept;
    /** Binds one native instruction; flag and value slots resolve to their saved bank. */
    [[nodiscard]] bool bind(std::uint32_t native,
                            std::uint32_t operand,
                            state::build_data::rewards::Instruction& instruction) const noexcept;
    /** Appends one expanded expression; the bank is unchanged on failure. */
    [[nodiscard]] bool read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<state::build_data::rewards::Instruction>& bank,
                            state::build_data::rewards::Range& range,
                            BankFailure& failure) const noexcept;
    /** Expands a list of expressions into one program where every expression must hold. */
    [[nodiscard]] bool read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<state::build_data::rewards::Instruction> output,
                                 std::size_t& count,
                                 BankFailure& failure) const noexcept;

private:
    [[nodiscard]] bool append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<state::build_data::rewards::Instruction>& bank,
                                         std::size_t depth,
                                         BankFailure& failure) const noexcept;
    /** Reads every class binding before any reward condition can be published. */
    [[nodiscard]] bool load_class_flags(const middleware::content::packages::reader::Source& source,
                                        middleware::content::packages::reader::Scratch& scratch,
                                        std::span<const std::byte> root) noexcept;
    std::array<std::uint16_t, state::kCharacterClassCount> classFlags_{};
    const SlotMaps* maps_{};
    bool loaded_{};
    std::vector<std::byte> flags_;
    std::vector<std::byte> values_;
    std::vector<std::byte> expressions_;
    middleware::content::packages::tables::Array flagRows_{};
    middleware::content::packages::tables::Array valueRows_{};
    middleware::content::packages::tables::Array expressionRows_{};
};

/** Reward metadata collected alongside the existing item-definition walk. */
class RewardBuild {
public:
    RewardConditions conditions;
    /** Reads every reward pool; unreadable entries are skipped, a full bank refuses the load. */
    [[nodiscard]] bool load(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept;
    /** Sizes the dense item rows before the item walk. */
    [[nodiscard]] bool begin_items(std::size_t count) noexcept;
    /** Records one item's wrapper and acquisition flag; an unreadable item stays unavailable. */
    void item(std::uint16_t index, std::uint32_t hash, std::span<const std::byte> blob) noexcept;
    /** Notes an item whose definition failed to read; publication waits for a clean walk. */
    void item_unread(std::uint16_t index) noexcept;
    /** Trims item rows to the catalog, drops rows naming unread items or pools, then publishes. */
    [[nodiscard]] bool publish(std::size_t catalogItemCount) noexcept;

private:
    /** Appends one entry; a bank failure also names the bank and what refused it. */
    [[nodiscard]] bool entry(std::span<const std::byte> blob,
                             std::size_t at,
                             BankFailure& failure,
                             const char*& reason) noexcept;
    [[nodiscard]] bool read_item(std::uint32_t hash,
                                 std::span<const std::byte> blob,
                                 state::build_data::rewards::Item& item) noexcept;
    bool loaded_{};
    bool supplementalMissing_{};
    std::size_t unboundAcquiredFlags_{};
    std::size_t discardedAcquiredFlags_{};
    std::size_t unreadItems_{};
    std::uint16_t firstUnreadItem_{};
    std::vector<state::build_data::rewards::Pool> pools_;
    std::vector<state::build_data::rewards::Entry> entries_;
    std::vector<state::build_data::rewards::Item> items_;
    std::vector<state::build_data::rewards::Instruction> instructions_;
    std::vector<state::build_data::rewards::Modifier> modifiers_;
    std::vector<state::build_data::rewards::SocketOverride> sockets_;
};

/** Progression rewards and pool entries use the same socket-override layout. */
[[nodiscard]] bool read_reward_sockets(std::span<const std::byte> blob,
                                       std::size_t at,
                                       std::span<state::build_data::rewards::SocketOverride> output,
                                       std::size_t& count) noexcept;

} // namespace sunrise::client::content::items::packages
