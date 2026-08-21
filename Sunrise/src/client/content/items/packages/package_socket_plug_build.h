#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/items/catalysts/definition.h"
#include "../../../../state/build_data/items/item_catalog.h"
#include "../../../../state/build_data/items/socket_plugs/definition.h"

namespace sunrise::client::content::items::packages {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace socket_plugs = state::build_data::items::socket_plugs;
namespace catalysts = state::build_data::items::catalysts;

/**
 * Reads the dense socket-type table and extracts each type's acquired-state gate.
 * @param source Installed package source.
 * @param scratch Shared package reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the socket-type table.
 * @param output Receives one row per native socket type.
 * @return True when the fixed table shape and every row extent are valid.
 */
[[nodiscard]] bool
read_catalyst_acquisition_gates(const reader::Source& source,
                                reader::Scratch& scratch,
                                std::span<const std::byte> root,
                                std::vector<std::byte>& blob,
                                std::vector<catalysts::AcquisitionGate>& output) noexcept;

/**
 * Reads the dense objective table's build-defined completion values.
 * @param source Installed package source.
 * @param scratch Shared package reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the objective table.
 * @param output Receives one completion value per native objective index.
 * @return True when the table class, row class, count, and fixed rows are valid.
 */
[[nodiscard]] bool
read_catalyst_objective_values(const reader::Source& source,
                               reader::Scratch& scratch,
                               std::span<const std::byte> root,
                               std::vector<std::byte>& blob,
                               std::vector<std::int32_t>& output) noexcept;

/**
 * Finds every positive flag and `value >= literal` term plus one objective reference.
 * Duplicate terms are folded. Conflicting objectives or fixed-capacity overflow fail closed.
 * @param definition Complete installed definition of the catalyst effect item.
 * @param itemDefinitionIndex Native index of the catalyst effect item.
 * @param output Receives the unique condition or its absent or ambiguous state.
 */
void read_catalyst_completion_condition(std::span<const std::byte> definition,
                                        std::uint16_t itemDefinitionIndex,
                                        catalysts::CompletionCondition& output) noexcept;

/** Fixed-size, heap-backed interning state for one installed package pass. */
class SocketPlugBuild final {
public:
    SocketPlugBuild() = default;
    ~SocketPlugBuild() = default;
    SocketPlugBuild(const SocketPlugBuild&) = delete;
    SocketPlugBuild& operator=(const SocketPlugBuild&) = delete;

    /**
     * Allocates bounded scratch and indexes the three native expandable plug categories.
     * @param specialCategories Category code for each native item index.
     * @param itemDefinitions Complete installed item table.
     * @return True when every bounded scratch bank is ready.
     */
    [[nodiscard]] bool
    prepare(std::span<const std::uint8_t> specialCategories,
            std::span<const state::build_data::items::Definition> itemDefinitions) noexcept;

    /** Extracts, expands, interns, and records every declared socket lane of one base item. */
    [[nodiscard]] bool append(const tables::items::Row& item,
                              std::span<const std::byte> itemDefinition,
                              std::span<const std::byte> plugSetTable,
                              std::size_t itemDefinitionCount) noexcept;

    /** Publishes the completed exact relation. The object retains source rows until destruction. */
    [[nodiscard]] bool publish() noexcept;

    /** @return Socket lanes skipped because their package lists were malformed or over capacity. */
    [[nodiscard]] std::size_t skipped() const noexcept;
    /** @return Number of extracted socket rules. */
    [[nodiscard]] std::size_t rule_count() const noexcept;
    /** @return Number of interned socket pools. */
    [[nodiscard]] std::size_t pool_count() const noexcept;
    /** @return Number of members in all interned socket pools. */
    [[nodiscard]] std::size_t member_count() const noexcept;
    /** @return Extracted socket rules, valid until this object changes or is destroyed. */
    [[nodiscard]] std::span<const socket_plugs::Rule> rules() const noexcept;
    /** @return Interned socket pools, valid until this object changes or is destroyed. */
    [[nodiscard]] std::span<const socket_plugs::Pool> pools() const noexcept;
    /** @return Interned socket members, valid until this object changes or is destroyed. */
    [[nodiscard]] std::span<const socket_plugs::Member> members() const noexcept;

    /** Package-list visitor entry point; accepts only an in-range bounded native index. */
    [[nodiscard]] bool add(std::uint32_t itemDefinitionIndex,
                           std::size_t itemDefinitionCount) noexcept;

private:
    struct PoolLookup {
        std::uint64_t fingerprint{};
        std::uint32_t poolIndex{UINT32_MAX};
    };

    /** A plug set names three categories: reusable, randomized, and the socket's own default. */
    static constexpr std::size_t kCategoryCount = 3;
    /** Power-of-two open-addressed table, sized so the largest package's pools stay sparse. */
    static constexpr std::size_t kLookupCapacity = 1U << 19U;

    std::vector<socket_plugs::Rule> rules_{};
    std::vector<socket_plugs::Pool> pools_{};
    std::vector<socket_plugs::Member> members_{};
    std::vector<socket_plugs::Member> candidates_{};
    std::vector<socket_plugs::Member> categoryMembers_{};
    std::vector<PoolLookup> lookup_{};
    std::array<std::size_t, kCategoryCount> categoryCounts_{};
    std::array<socket_plugs::Member, 3> trackerMembers_{};
    std::size_t trackerCount_{};
    std::size_t ruleCount_{};
    std::size_t poolCount_{};
    std::size_t memberCount_{};
    std::size_t candidateCount_{};
    std::size_t skipped_{};

    /** Expands native category families, canonicalizes, and interns the current candidate. */
    [[nodiscard]] bool intern(std::uint32_t& poolIndex) noexcept;
    /** Releases every transient allocation and count. */
    void release() noexcept;
};

/**
 * Classifies the only three plug-category hashes whose native socket rules expand by category.
 * @return 1..3 for a supported expansion family, or zero otherwise.
 */
[[nodiscard]] std::uint8_t special_plug_category(std::uint32_t categoryHash) noexcept;

} // namespace sunrise::client::content::items::packages
