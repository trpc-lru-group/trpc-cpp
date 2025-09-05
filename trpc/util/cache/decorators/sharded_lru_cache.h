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

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <type_traits>
#include <utility>

#include "../impl/basic_cache.h"
#include "lru_cache.h"
namespace trpc::cache {

/// @brief This LRU cache, based on bitwise sharding (2^ShardBits), implements approximate global LRU and strict LRU
/// within shards. Each shard consists of an `LRUCache` (decorator) + a pluggable base (default `BasicCache`). Shards
/// are not locked, improving concurrency.
///
/// Key Design Points:
/// 1) Sharding: Number of shards = 2^ShardBits. Shards are selected using Hash & (N-1);
/// 2) Capacity: Total capacity is evenly divided among shards, with the remainder allocated to the first few shards;
///    each shard has a capacity of at least 1;
/// 3) Thread-Safety: LRUCache's Mutex ensures intra-shard locks; no global
///    locks are required between shards;
/// 4) Pluggable Base: A factory function constructs an underlying cache for each
///    shard (default BasicCache).
///
/// @tparam KeyType key type
/// @tparam ValueType value type
/// @tparam HashFn hash function (default std::hash)
/// @tparam KeyEqual equality comparer (default std::equal_to)
/// @tparam Mutex mutex type within the shard (default std::mutex; fiber mutex can be specified)
/// @tparam ShardBits number of shard bits, number of shards = 2^ShardBits (e.g., 4 -> 16 shards)
///
/// Usage example:
///   using CacheT = trpc::cache::ShardedLRUCache<std::string, std::string, std::hash<std::string>,
///                                               std::equal_to<std::string>, std::mutex, 4>;
///   CacheT c(/*capacity*/ 1<<20);  // Total capacity, automatically divided by shards
///
template <typename KeyType, typename ValueType, typename HashFn = std::hash<KeyType>,
          typename KeyEqual = std::equal_to<KeyType>, typename Mutex = std::mutex, std::size_t ShardBits = 4>
class ShardedLRUCache final : public Cache<KeyType, ValueType, HashFn, KeyEqual> {
  static_assert(ShardBits < (8 * sizeof(std::size_t)), "ShardBits is too large for size_t");
  static constexpr std::size_t kNumShards = static_cast<std::size_t>(1) << ShardBits;
  static_assert((kNumShards & (kNumShards - 1)) == 0 && kNumShards >= 1, "Number of shards must be a power of two");

  using BaseCache = Cache<KeyType, ValueType, HashFn, KeyEqual>;
  using LruShard = LRUCache<KeyType, ValueType, HashFn, KeyEqual, Mutex>;
  using ShardPtr = std::unique_ptr<LruShard>;
  using FactoryFn = std::function<std::unique_ptr<BaseCache>()>;

 public:
  /// @brief Constructor
  /// @param capacity Global capacity (to be split across shards; at least 1 per shard)
  /// @param factory Shard base factory (by default, creates a BasicCache<Key,Value>)
  explicit ShardedLRUCache(
      std::size_t capacity,
      FactoryFn factory = []() { return std::make_unique<BasicCache<KeyType, ValueType, HashFn, KeyEqual>>(); })
      : total_capacity_(capacity) {
    if (total_capacity_ == 0) {
      // Avoid shard construction exceptions caused by 0 capacity
      total_capacity_ = kNumShards;
    }

    const std::size_t base = total_capacity_ / kNumShards;
    std::size_t rem = total_capacity_ % kNumShards;

    for (std::size_t i = 0; i < kNumShards; ++i) {
      std::size_t shard_cap = base + (rem ? 1 : 0);
      if (rem) --rem;
      if (shard_cap == 0) shard_cap = 1;  // At least 1 per shard

      shards_[i] = std::make_unique<LruShard>(factory(), shard_cap);
    }
  }

  ~ShardedLRUCache() override = default;

  ShardedLRUCache(const ShardedLRUCache&) = delete;
  ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

  ShardedLRUCache(ShardedLRUCache&&) noexcept = default;
  ShardedLRUCache& operator=(ShardedLRUCache&&) noexcept = default;

  /// @brief Put (copy semantics)
  bool Put(const KeyType& key, const ValueType& value) override { return ShardOf(key).Put(key, value); }

  /// @brief Put (move semantics)
  bool Put(const KeyType& key, ValueType&& value) override {
    return ShardOf(key).Put(key, std::forward<ValueType>(value));
  }

  /// @brief Get
  std::optional<ValueType> Get(const KeyType& key) override { return ShardOf(key).Get(key); }

  /// @brief Remove
  bool Remove(const KeyType& key) override { return ShardOf(key).Remove(key); }

  /// @brief Clear (clear all shards)
  void Clear() override {
    for (auto& s : shards_) {
      s->Clear();
    }
  }

  /// @brief Size (sum of the sizes of all shards; approximate real-time value)
  std::size_t Size() override {
    std::size_t sum = 0;
    for (auto& s : shards_) {
      sum += s->Size();
    }
    return sum;
  }

  /// @brief Returns the total number of shards
  static constexpr std::size_t NumShards() { return kNumShards; }

  /// @brief Returns the global capacity (specified during construction)
  std::size_t Capacity() const { return total_capacity_; }

 private:
  // Select shard (bitwise sharding, requires 2^ShardBits)
  inline std::size_t ShardIndex(const KeyType& key) const {
    // HashFn needs to be stateless or default constructible; 
    // if stateful hashing is required, it can be externally encapsulated
    const std::size_t h = hasher_(key);
    return h & (kNumShards - 1);
  }

  inline LruShard& ShardOf(const KeyType& key) { return *shards_[ShardIndex(key)]; }
  inline const LruShard& ShardOf(const KeyType& key) const { return *shards_[ShardIndex(key)]; }

 private:
  std::size_t total_capacity_;
  std::array<ShardPtr, kNumShards> shards_{};
  HashFn hasher_{};
};

}  // namespace trpc::cache
