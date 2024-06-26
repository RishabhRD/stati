#pragma once

#include "cache/concepts.hpp"
#include "page/page.hpp"
#include "page/page_id.hpp"
#include "std/ranges.hpp"
#include "storage/concepts.hpp"
#include <algorithm>
#include <optional>
#include <stdexec/exec/task.hpp>
#include <vector>

namespace statdb {

template <Cache cache, PageStorage page_storage> class buffer_pool {
  cache eviction_cache;
  std::vector<page> page_frames;
  std::vector<std::optional<page_id>> page_ids;
  std::vector<std::size_t> acquire_cnt;
  page_storage storage_;

public:
  // Class Invariants:
  //   - page_frames.size() == page_ids.size() == acquire_cnt.size()
  //   - if(acquire_cnt[i] > 0)
  //         -> eviction_cache.contains(i) == false;
  //     else
  //         -> true;

  buffer_pool(std::size_t pool_size, page_storage storage)
      : eviction_cache(pool_size), page_frames(pool_size), page_ids(pool_size),
        acquire_cnt(pool_size), storage_(std::move(storage)) {
    for (std::size_t i{}; i < capacity(); ++i) {
      eviction_cache.add(i);
    }
  }

  auto capacity() const noexcept { return page_ids.size(); }

  // Precondition:
  //   - If page exists in cache, return that page with incrementing the
  //     acquire_cnt for that page
  //   - Otherwise, if cache can evict some frame and storage has the given
  //     page, then evict that frame and replace frame page with new page and
  //     increase the acquire count
  auto acquire(page_id id) -> exec::task<page const *> {
    auto const page_idx = static_cast<std::size_t>(
        rng::distance(rng::begin(page_ids), rng::find(page_ids, id)));
    if (page_idx != std::size(page_ids)) {
      co_return acquire_page_from_cache(page_idx);
    } else {
      co_return co_await acquire_page_from_storage(id);
    }
  }

  // Precondition:
  //   - id is a page_id existing in pool
  // Postcondition:
  //   - decrement acquire[cnt] of index of page id by 1
  auto release(page_id id) {
    auto const page_idx = static_cast<std::size_t>(
        rng::distance(rng::begin(page_ids), rng::find(page_ids, id)));
    if ((--acquire_cnt[page_idx]) == 0) {
      eviction_cache.add(page_idx);
    }
  }

private:
  auto acquire_page_from_cache(std::size_t page_idx) {
    if (acquire_cnt[page_idx]++ == 0) {
      eviction_cache.remove(page_idx);
    }
    return &page_frames[page_idx];
  }

  auto acquire_page_from_storage(page_id id) -> exec::task<page const *> {
    if (eviction_cache.size() == 0)
      co_return static_cast<page const *>(nullptr);

    auto page_idx = eviction_cache.peek();
    auto did_read_page =
        co_await storage_.fetch_page(id, page_frames[page_idx]);
    if (!did_read_page) {
      co_return static_cast<page const *>(nullptr);
    }
    eviction_cache.evict();
    ++acquire_cnt[page_idx];
    page_ids[page_idx] = std::move(id);
    co_return &page_frames[page_idx];
  }
};

} // namespace statdb
