#pragma once

#include <mutex>
#include <utility>

#include <gsl/pointers>
#include <stdx/fixed/hash_table.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <vector>

#include "storage/disk_manager.hh"
#include "storage/error.hh"
#include "storage/frame_replacer.hh"
#include "storage/page.hh"

namespace cairn::storage {

template <usize PoolSize> class buffer_pool;

// RAII holding a shared latch on a pinned page
template <usize PoolSize>
    requires(PoolSize > 0)
class read_guard {
  public:
    constexpr read_guard() noexcept = default;
    read_guard(buffer_pool<PoolSize>& pool, page& pg) noexcept : pool_{pool}, pg_{pg} {}
    ~read_guard() { drop(); }

    read_guard(const read_guard&)                    = delete;
    auto operator=(const read_guard&) -> read_guard& = delete;

    read_guard(read_guard&& other) noexcept : pool_{other.pool_}, pg_{other.pg_} {
        other.pool_.reset();
        other.pg_.reset();
    }

    auto operator=(read_guard&& other) noexcept -> read_guard& {
        if (this != &other) {
            drop();
            pool_ = other.pool_;
            pg_   = other.pg_;
            other.pool_.reset();
            other.pg_.reset();
        }
        return *this;
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return pool_.has_value(); }
    [[nodiscard]] auto page_id() const noexcept -> page_id_t { return pg_->page_id(); }

    // Read-only view of the page's bytes
    template <typename T> [[nodiscard]] auto as() const noexcept -> const T* {
        return stdx::object_at<T>(pg_->data());
    }

    // Called automatically on destruction but safe to call manually
    auto drop() noexcept -> void {
        if (pool_) {
            const auto pid{pg_->page_id()};
            pg_->latch().unlock_shared();
            DISCARD(pool_->unpin_page(pid, false));
            pool_.reset();
            pg_.reset();
        }
    }

  private:
    stdx::option<buffer_pool<PoolSize>&> pool_;
    stdx::option<page&>                  pg_;
};

// RAII holding an exclusive latch on a pinned page
template <usize PoolSize>
    requires(PoolSize > 0)
class write_guard {
  public:
    constexpr write_guard() noexcept = default;
    write_guard(buffer_pool<PoolSize>& pool, page& pg) noexcept : pool_{pool}, pg_{pg} {}
    ~write_guard() { drop(); }

    write_guard(const write_guard&)                    = delete;
    auto operator=(const write_guard&) -> write_guard& = delete;

    write_guard(write_guard&& other) noexcept
        : pool_{other.pool_}, pg_{other.pg_}, dirty_{other.dirty_} {
        other.pool_.reset();
        other.pg_.reset();
    }

    auto operator=(write_guard&& other) noexcept -> write_guard& {
        if (this != &other) {
            drop();
            pool_  = other.pool_;
            pg_    = other.pg_;
            dirty_ = other.dirty_;
            other.pool_.reset();
            other.pg_.reset();
        }
        return *this;
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return pool_.has_value(); }
    [[nodiscard]] auto page_id() const noexcept -> page_id_t { return pg_->page_id(); }
    auto               mark_dirty() noexcept -> void { dirty_ = true; }

    template <typename T> [[nodiscard]] auto as(this auto&& self) noexcept -> auto* {
        return stdx::object_at<T>(self.pg_->data());
    }

    // Called automatically on destruction but safe to call manually
    auto drop() noexcept -> void {
        if (pool_) {
            const auto pid{pg_->page_id()};
            pg_->latch().unlock();
            DISCARD(pool_->unpin_page(pid, dirty_));
            pool_.reset();
            pg_.reset();
        }
    }

  private:
    stdx::option<buffer_pool<PoolSize>&> pool_;
    stdx::option<page&>                  pg_;
    bool                                 dirty_{false};
};

// Manager and dispatcher of concurrent frames from a disk manager.
template <usize PoolSize> class buffer_pool {
  public:
    using read_guard_t  = read_guard<PoolSize>;
    using write_guard_t = write_guard<PoolSize>;

  public:
    explicit buffer_pool() {}
    ~buffer_pool() {}

    MAKE_PINNED(buffer_pool);

    [[nodiscard]] static constexpr auto size() noexcept -> usize { return PoolSize; }

    // Pins the provided pid and returns a stable pointer
    [[nodiscard]] auto fetch_page(page_id_t pid) -> result<gsl::not_null<page*>> { TODO(pid); }

    // Allocates a fresh zeroed page
    [[nodiscard]] auto new_page() -> result<gsl::not_null<page*>> { TODO(); }

    [[nodiscard]] auto unpin_page(page_id_t pid, bool dirty) -> result<void> { TODO(pid, dirty); }

    // Flush a specific frame in the pool
    [[nodiscard]] auto flush(page_id_t pid) -> result<void> { TODO(pid); }

    // Flushes all frames in the buffer
    [[nodiscard]] auto flush() -> result<void> { TODO(); }

    [[nodiscard]] auto delete_page(page_id_t pid) -> result<void> { TODO(pid); }

    [[nodiscard]] auto fetch_read(page_id_t pid) -> result<read_guard_t> { TODO(pid); }

    [[nodiscard]] auto fetch_write(page_id_t pid) -> result<write_guard_t> { TODO(pid); }

    [[nodiscard]] auto new_write() -> result<std::pair<page_id_t, write_guard_t>> { TODO(); }

    [[nodiscard]] auto pin_count(page_id_t pid) -> stdx::option<i32> { TODO(pid); }

    [[nodiscard]] auto is_resident(page_id_t pid) -> bool { TODO(pid); }

    [[nodiscard]] auto evictable_count() -> usize { TODO(); }

  private:
    static constexpr usize TABLE_CAPACITY{PoolSize * 2};
    static constexpr usize REHASH_CHURN_LIMIT{PoolSize / 2 == 0 ? 1UZ : PoolSize / 2};

  private:
    [[nodiscard]] auto frame_at(frame_id_t fid) noexcept -> page& { TODO(fid); }

    [[nodiscard]] auto grab_victim_frame() -> result<frame_id_t> { TODO(); }

    // Caller needs to hold the mutex as this manages manual rehashing
    auto note_table_removal() -> void { TODO(); }

    [[nodiscard]] auto flush_locked(frame_id_t fid) -> result<void> { TODO(fid); }

  private:
    std::mutex                                                   mutex_;
    stdx::box<disk_manager>                                      disk_;
    stdx::box<page[]>                                            frames_;
    frame_replacer<PoolSize>                                     replacer_;
    stdx::fixed::vector<frame_id_t, PoolSize>                    free_list_;
    std::vector<page_id_t>                                       free_pages_;
    usize                                                        page_table_churn_{0};
    stdx::fixed::hash_map<page_id_t, frame_id_t, TABLE_CAPACITY> page_table_;
};

} // namespace cairn::storage
