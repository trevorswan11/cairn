#pragma once

#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

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
    template <typename T> [[nodiscard]] auto as() const noexcept -> gsl::not_null<const T*> {
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
    explicit buffer_pool(stdx::box<disk_manager> dm) : disk_{std::move(dm)} {
        frames_ = stdx::make_box<page[]>(PoolSize);
        for (usize i{0}; i < PoolSize; ++i) {
            free_list_.emplace_back(static_cast<frame_id_t>(PoolSize - 1 - i));
        }
    }
    ~buffer_pool() { VERIFY(flush()); }
    MAKE_PINNED(buffer_pool);

    [[nodiscard]] static auto open(const std::filesystem::path& path)
        -> result<stdx::box<buffer_pool>> {
        auto dm{TRY(disk_manager::open(path))};
        return stdx::make_box<buffer_pool>(std::move(dm));
    }

    [[nodiscard]] static constexpr auto size() noexcept -> usize { return PoolSize; }

    // Pins the provided pid and returns a stable pointer
    [[nodiscard]] auto fetch_page(page_id_t pid) -> result<gsl::not_null<page*>> {
        std::scoped_lock lock{mutex_};

        if (auto slot{page_table_.get_opt(pid)}) {
            const auto fid{*slot};
            auto&      f{frame_at(fid)};
            f.pin();
            replacer_.access(fid);
            replacer_.set_evictable(fid, false);
            return &f;
        }

        const auto fid{TRY(grab_victim_frame())};
        auto&      f{frame_at(fid)};
        f.reset(pid);

        if (auto r{disk_->read_page(pid, f.data())}; !r) {
            free_list_.emplace_back(fid);
            return stdx::err{r.error()};
        }

        f.pin();
        page_table_.emplace(pid, fid);
        replacer_.access(fid);
        replacer_.set_evictable(fid, false);
        return &f;
    }

    // Allocates a fresh zeroed page
    [[nodiscard]] auto new_page() -> result<gsl::not_null<page*>> {
        std::scoped_lock lock{mutex_};
        const auto       fid{TRY(grab_victim_frame())};

        auto pid{INVALID_PAGE_ID};
        if (!free_pages_.empty()) {
            pid = free_pages_.back();
            free_pages_.pop_back();
        } else {
            auto allocated{disk_->allocate_page()};
            if (!allocated) {
                free_list_.emplace_back(fid);
                return stdx::err{allocated.error()};
            }
            pid = *allocated;
        }

        auto& f{frame_at(fid)};
        f.reset(pid);

        f.set_dirty(true);
        f.pin();
        page_table_.emplace(pid, fid);
        replacer_.access(fid);
        replacer_.set_evictable(fid, false);
        return &f;
    }

    [[nodiscard]] auto unpin_page(page_id_t pid, bool dirty) -> result<void> {
        std::scoped_lock lock{mutex_};
        auto             slot{page_table_.get_opt(pid)};
        if (!slot) { return stdx::err{error_t::PAGE_NOT_FOUND}; }

        const auto fid{*slot};
        auto&      f{frame_at(fid)};
        if (f.pin_count() <= 0) { return stdx::err{error_t::PAGE_NOT_FOUND}; }
        if (dirty) { f.set_dirty(true); }
        if (f.unpin() == 0) { replacer_.set_evictable(fid, true); }
        return {};
    }

    // Flush a specific frame in the pool
    [[nodiscard]] auto flush(page_id_t pid) -> result<void> {
        std::scoped_lock lock{mutex_};
        auto             slot{page_table_.get_opt(pid)};
        if (!slot) { return stdx::err{error_t::PAGE_NOT_FOUND}; }
        return flush_locked(*slot);
    }

    // Flushes all frames in the buffer
    [[nodiscard]] auto flush() -> result<void> {
        std::scoped_lock lock{mutex_};
        for (usize i{0}; i < PoolSize; ++i) { TRY(flush_locked(static_cast<frame_id_t>(i))); }
        return {};
    }

    // Evicts the provides pid unless it isn't resident in which case this is a noop
    [[nodiscard]] auto delete_page(page_id_t pid) -> result<void> {
        std::scoped_lock lock{mutex_};
        auto             slot{page_table_.get_opt(pid)};
        if (!slot) { return {}; }

        const auto fid{*slot};
        auto&      f{frame_at(fid)};
        if (f.pin_count() > 0) { return stdx::err{error_t::TREE_CORRUPT}; }

        page_table_.remove(pid);
        note_table_removal();
        replacer_.remove(fid);
        f.reset(INVALID_PAGE_ID);
        free_list_.emplace_back(fid);
        free_pages_.emplace_back(pid);
        return {};
    }

    [[nodiscard]] auto fetch_read(page_id_t pid) -> result<read_guard_t> {
        auto pg{TRY(fetch_page(pid))};
        pg->latch().lock_shared();
        return read_guard_t{*this, *pg};
    }

    [[nodiscard]] auto fetch_write(page_id_t pid) -> result<write_guard_t> {
        auto pg{TRY(fetch_page(pid))};
        pg->latch().lock();
        return write_guard_t{*this, *pg};
    }

    [[nodiscard]] auto new_write() -> result<std::pair<page_id_t, write_guard_t>> {
        auto pg{TRY(new_page())};
        pg->latch().lock();
        return std::make_pair(pg->page_id(), write_guard_t{*this, *pg});
    }

    [[nodiscard]] auto pin_count(page_id_t pid) -> stdx::option<i32> {
        std::scoped_lock lock{mutex_};
        auto             slot{page_table_.get_opt(pid)};
        if (!slot) { return stdx::none; }
        return frame_at(*slot).pin_count();
    }

    [[nodiscard]] auto is_resident(page_id_t pid) -> bool {
        std::scoped_lock lock{mutex_};
        return page_table_.contains(pid);
    }

    [[nodiscard]] auto evictable_count() -> usize {
        std::scoped_lock lock{mutex_};
        return replacer_.size();
    }

  private:
    static constexpr usize TABLE_CAPACITY{PoolSize * 2};
    static constexpr usize REHASH_CHURN_LIMIT{PoolSize / 2 == 0 ? 1UZ : PoolSize / 2};

  private:
    [[nodiscard]] auto frame_at(frame_id_t fid) noexcept -> page& {
        return frames_[static_cast<usize>(std::to_underlying(fid))];
    }

    // Caller needs to hold the mutex
    [[nodiscard]] auto grab_victim_frame() -> result<frame_id_t> {
        if (!free_list_.empty()) {
            const auto fid{free_list_.back()};
            free_list_.pop_back();
            return fid;
        }

        auto victim{replacer_.evict()};
        if (!victim) { return stdx::err{error_t::POOL_EXHAUSTED}; }

        const auto fid{*victim};
        auto&      f{frame_at(fid)};
        if (f.is_dirty()) {
            // If the frame cannot persist then it must stay resident
            if (auto r{disk_->write_page(f.page_id(), f.data())}; !r) {
                replacer_.access(fid);
                replacer_.set_evictable(fid, true);
                return stdx::err{r.error()};
            }
            f.set_dirty(false);
        }
        page_table_.remove(f.page_id());
        note_table_removal();
        return fid;
    }

    // Caller needs to hold the mutex as this manages manual rehashing
    auto note_table_removal() -> void {
        if (++page_table_churn_ >= REHASH_CHURN_LIMIT) {
            page_table_.rehash();
            page_table_churn_ = 0;
        }
    }

    [[nodiscard]] auto flush_locked(frame_id_t fid) -> result<void> {
        auto& f{frame_at(fid)};
        if (!f.is_dirty()) { return {}; }
        TRY(disk_->write_page(f.page_id(), f.data()));
        f.set_dirty(false);
        return {};
    }

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
