/*
 *
 * Tencent is pleased to support the open source community by making
 * tRPC available.
 *
 * Copyright (C) 2025 Tencent.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "trpc/util/cache/decorators/sharded_lru_cache.h"
#include "trpc/util/cache/impl/basic_cache.h"

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace trpc::cache::testing {

namespace {
// Make shard positioning controllable: hash value = key value (easy to route to the target shard by bit)
struct IdentityHash {
  size_t operator()(int k) const noexcept { return static_cast<size_t>(k); }
};
}  // namespace

// ---------- Basic capabilities (aligned with single-shard LRU use case) ----------

/// @brief Move semantics: data remains intact and of the same size after the move
TEST(ShardedLRUCacheTest, MoveSemantics) {
  // ShardBits = 2 => 4 shards; total capacity 16
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache1(16);
  cache1.Put(1, 1);
  ASSERT_EQ(cache1.Get(1), 1);

  auto cache2 = std::move(cache1);
  ASSERT_EQ(cache2.Get(1), 1);
  ASSERT_EQ(cache2.Size(), 1);
}

/// @brief Put/Get/Size basic functions
TEST(ShardedLRUCacheTest, BasicPutGetSize) {
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache(8);

  ASSERT_EQ(cache.Size(), 0);
  ASSERT_TRUE(cache.Put(1, 1));
  ASSERT_EQ(cache.Get(1), 1);
  ASSERT_EQ(cache.Size(), 1);
}

/// @brief Duplicate key insert: value should be updated and returns false, size unchanged
TEST(ShardedLRUCacheTest, PutDuplicateKey) {
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache(4);

  ASSERT_TRUE(cache.Put(1, 1));
  ASSERT_FALSE(cache.Put(1, 2));  // BasicCache::InsertOrAssign returns false if it already exists
  ASSERT_EQ(cache.Size(), 1);
  ASSERT_EQ(cache.Get(1), 2);
}

/// @brief Delete an existing key
TEST(ShardedLRUCacheTest, RemoveExistingKey) {
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache(4);

  cache.Put(1, 1);
  ASSERT_TRUE(cache.Remove(1));
  ASSERT_EQ(cache.Get(1), std::nullopt);
  ASSERT_EQ(cache.Size(), 0);
}

/// @brief Delete non-existent keys
TEST(ShardedLRUCacheTest, RemoveNonExistingKey) {
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache(4);

  ASSERT_FALSE(cache.Remove(1));
  ASSERT_EQ(cache.Size(), 0);
}

/// @brief Clear：Clear all shards
TEST(ShardedLRUCacheTest, ClearCache) {
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 2> cache(4);

  cache.Put(1, 1);
  cache.Put(2, 2);
  cache.Clear();

  ASSERT_EQ(cache.Size(), 0);
  ASSERT_EQ(cache.Get(1), std::nullopt);
  ASSERT_EQ(cache.Get(2), std::nullopt);
}

// ---------- Sharding semantics (core: strict LRU within a slice, independence between slices) ----------

/// @brief Single-shard strict LRU: Concentrate capacity on the same shard, and verify that eviction is based on the LRU
///        within the shard.
TEST(ShardedLRUCacheTest, PerShardEvictionRespectsLRU) {
  // 2 shards (even shards on shard0, odd shards on shard1), total capacity = 4 => capacity per shard = 2
  using ShardedCache = ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 1>;
  ShardedCache cache(4);

  // First fill two shards: shard0 with even numbers (0,2), shard1 with odd numbers (1,3)
  cache.Put(0, 0);  // shard0
  cache.Put(2, 2);  // shard0
  cache.Put(1, 1);  // shard1
  cache.Put(3, 3);  // shard1

  // Access 0 is promoted to the MRU of shard0, the order within the shard (MRU->LRU): 0, 2
  ASSERT_EQ(cache.Get(0), 0);

  // Insert another key 4 into shard0, triggering shard0 to evict LRU(2), without affecting shard1(1,3)
  cache.Put(4, 4);  // shard0

  ASSERT_EQ(cache.Get(2), std::nullopt);  // Evicted (on-shard LRU)
  ASSERT_EQ(cache.Get(0), 0);             // Still in（MRU）
  ASSERT_EQ(cache.Get(4), 4);             // New Insert

  // Shard 1 is not affected
  ASSERT_EQ(cache.Get(1), 1);
  ASSERT_EQ(cache.Get(3), 3);

  // The current total size should be 4 (both shards are full)
  ASSERT_EQ(cache.Size(), 4u);
}

/// @brief Single-shard degenerate (ShardBits=0): Behavior equivalent to "global single-shard LRU", convenient for
///        aligning single-shard use cases
TEST(ShardedLRUCacheTest, SingleShardBehavesLikeGlobalLRU) {
  // Single shard: capacity = 3, equivalent to a single LRU capacity = 3
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 0> cache(3);

  cache.Put(1, 1);
  cache.Put(2, 2);
  cache.Put(3, 3);

  // Access 1 promoted to MRU
  ASSERT_EQ(cache.Get(1), 1);

  // Insert 4th -> Evict LRU(2)
  cache.Put(4, 4);

  ASSERT_EQ(cache.Get(2), std::nullopt);  // Evicted
  ASSERT_EQ(cache.Get(1), 1);
  ASSERT_EQ(cache.Get(3), 3);
  ASSERT_EQ(cache.Get(4), 4);
  ASSERT_EQ(cache.Size(), 3u);
}

// ---------- Concurrent scenarios ----------

/// @brief High Concurrency Put: Verify consistency (Hits == Size)
TEST(ShardedLRUCacheTest, ConcurrentPut) {
  constexpr int capacity = 1000;
  // Multiple shards (8 shards), shared lock contention
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 3> cache(capacity);

  constexpr int threads_num = 8;
  constexpr int puts_per_thread = 200;
  constexpr int puts_num = threads_num * puts_per_thread;

  std::vector<std::thread> threads;
  std::atomic<int> key_gen{0};

  for (int i = 0; i < threads_num; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < puts_per_thread; ++j) {
        int key = key_gen.fetch_add(1, std::memory_order_relaxed);
        cache.Put(key, key);
      }
    });
  }
  for (auto& t : threads) t.join();

  int hits{0}, misses{0};
  for (int key = 0; key < puts_num; ++key) {
    if (cache.Get(key) == std::nullopt) {
      ++misses;
    } else {
      ++hits;
    }
  }

  ASSERT_EQ(hits, static_cast<int>(cache.Size()));
  ASSERT_EQ(misses, puts_num - static_cast<int>(cache.Size()));
}

/// @brief Highly concurrent Get: Maintaining correctness in both hit and miss scenarios
TEST(ShardedLRUCacheTest, ConcurrentGet) {
  constexpr int capacity = 1000;
  ShardedLRUCache<int, int, IdentityHash, std::equal_to<int>, std::mutex, 3> cache(capacity);

  for (int key = 0; key < capacity; ++key) {
    cache.Put(key, key);
  }

  constexpr int threads_num = 8;
  constexpr int gets_per_thread = 200;
  std::vector<std::thread> threads;

  for (int i = 0; i < threads_num; ++i) {
    threads.emplace_back([&, i]() {
      for (int j = 0; j < gets_per_thread; ++j) {
        int key = j + i * gets_per_thread;
        if (key < capacity) {
          ASSERT_EQ(cache.Get(key), key);
        } else {
          ASSERT_EQ(cache.Get(key), std::nullopt);
        }
      }
    });
  }
  for (auto& t : threads) t.join();
}

}  // namespace trpc::cache::testing
