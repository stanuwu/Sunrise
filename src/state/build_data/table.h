#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace sunrise::state::build_data {

/**
 * Reader/writer lock guarding one domain's published rows.
 * The lock sits apart from the storage so a domain with several arrays holds one lock across all
 * of them. A reader then never sees one array replaced and another not.
 */
class Lock final {
public:
    /** Holds the lock for reading until it leaves scope. */
    class Shared final {
    public:
        explicit Shared(const Lock& owner) noexcept : owner_(owner) {
            AcquireSRWLockShared(&owner_.lock_);
        }
        ~Shared() {
            ReleaseSRWLockShared(&owner_.lock_);
        }
        Shared(const Shared&) = delete;
        Shared(Shared&&) = delete;
        Shared& operator=(const Shared&) = delete;
        Shared& operator=(Shared&&) = delete;

    private:
        const Lock& owner_;
    };

    /** Holds the lock for writing until it leaves scope. */
    class Exclusive final {
    public:
        explicit Exclusive(Lock& owner) noexcept : owner_(owner) {
            AcquireSRWLockExclusive(&owner_.lock_);
        }
        ~Exclusive() {
            ReleaseSRWLockExclusive(&owner_.lock_);
        }
        Exclusive(const Exclusive&) = delete;
        Exclusive(Exclusive&&) = delete;
        Exclusive& operator=(const Exclusive&) = delete;
        Exclusive& operator=(Exclusive&&) = delete;

    private:
        Lock& owner_;
    };

private:
    // The acquire calls take a non-const pointer, and a shared hold does not modify the rows.
    mutable SRWLOCK lock_{SRWLOCK_INIT};
};

/**
 * Fixed row storage for one published table.
 * The caller must already hold the domain's Lock: exclusive to write, shared to read. Storage is
 * a member array, so the table lives in State for the whole process and nothing here allocates.
 * @tparam Row Trivially copyable published row.
 * @tparam Capacity Most rows the domain can publish.
 */
template <typename Row, std::size_t Capacity> class Table final {
public:
    /** Most rows this table accepts. */
    static constexpr std::size_t capacity = Capacity;

    /** Drops every row. Call under an exclusive hold. */
    void clear() noexcept {
        rows_.fill(Row{});
        count_ = 0;
    }

    /**
     * Replaces every row. Call under an exclusive hold, and check the rows first.
     * @param rows Complete replacement set, already checked by the domain.
     * @return True when the rows fit fixed storage.
     */
    [[nodiscard]] bool replace(std::span<const Row> rows) noexcept {
        if (rows.size() > Capacity) {
            return false;
        }
        rows_.fill(Row{});
        std::copy(rows.begin(), rows.end(), rows_.begin());
        // The count publishes the new rows, so it moves last.
        count_ = rows.size();
        return true;
    }

    /**
     * Clears the table, sizes it, and hands back writable storage. Call under an exclusive hold.
     * For a domain that places each row at a position the row itself names, not in input order.
     * No reader can see the sized but unwritten rows: writes hold exclusive, reads hold shared.
     * @param count Number of rows about to be written.
     * @return Storage for exactly that many rows, or an empty span when the count does not fit.
     */
    [[nodiscard]] std::span<Row> reset(std::size_t count) noexcept {
        if (count > Capacity) {
            return {};
        }
        rows_.fill(Row{});
        count_ = count;
        return {rows_.data(), count_};
    }

    /**
     * Copies every row in publish order. Call under a shared hold.
     * @param output Caller-owned storage.
     * @param count Receives the copied row count, or zero when output is too small.
     * @return True when output can hold every row.
     */
    [[nodiscard]] bool snapshot(std::span<Row> output, std::size_t& count) const noexcept {
        count = 0;
        if (output.size() < count_) {
            return false;
        }
        std::copy_n(rows_.begin(), count_, output.begin());
        count = count_;
        return true;
    }

    /** @return Published rows in publish order. Call under a shared hold. */
    [[nodiscard]] std::span<const Row> rows() const noexcept {
        return {rows_.data(), count_};
    }

    /** @return Published row count. Call under a shared hold. */
    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

private:
    std::array<Row, Capacity> rows_{};
    std::size_t count_{};
};

} // namespace sunrise::state::build_data
