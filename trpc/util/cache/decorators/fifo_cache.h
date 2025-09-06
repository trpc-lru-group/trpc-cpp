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

#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "trpc/util/cache/cache.h"
#include "trpc/util/concurrency/lightly_concurrent_hashmap.h"

namespace trpc::cache {

/// @brief A FIFO (First-In-First-Out) Cache implementation that evicts the oldest entry upon exceeding capacity.
///        This class decorates another Cache implementation (e.g., BasicCache) and adds FIFO eviction logic.
///        Thread-safety is achieved using a mutex to guard all operations.
///
/// @tparam KeyType   Type of the cache keys.
/// @tparam ValueType Type of the cache values.
/// @tparam HashFn    Hash function for keys (default: std::hash<KeyType>).
/// @tparam KeyEqual  Key equality comparator (default: std::equal_to<KeyType>).
/// @tparam Mutex     Mutex type for synchronization (default: std::mutex).
template <typename KeyType, typename ValueType, typename HashFn = std::hash<KeyType>,
          typename KeyEqual = std::equal_to<KeyType>, typename Mutex = std::std::timed_mutex>
class FIFOCache final : public Cache<KeyType, ValueType, HashFn, KeyEqual> {
 public:
  /// @brief Constructs a FIFO Cache with a wrapped cache instance and a maximum capacity.
  /// @param cache    The underlying cache implementation to decorate.
  /// @param capacity Maximum number of entries allowed in the cache.
  explicit FIFOCache(std::unique_ptr<Cache<KeyType, ValueType, HashFn, KeyEqual>> cache, size_t capacity = 1024)
      : capacity_(capacity), cache_(std::move(cache)) {}

  ~FIFOCache() = default;

  // Disable copying
  FIFOCache(const FIFOCache&) = delete;
  FIFOCache& operator=(const FIFOCache&) = delete;

  // Allow moving
  FIFOCache(FIFOCache&& other) noexcept
      : capacity_(other.capacity_),
        cache_(std::move(other.cache_)),
        fifo_list_(std::move(other.fifo_list_)),
        key_iter_map_(std::move(other.key_iter_map_)) {}

  FIFOCache& operator=(FIFOCache&& other) noexcept {
    if (this != &other) {
      capacity_ = other.capacity_;
      cache_ = std::move(other.cache_);
      fifo_list_ = std::move(other.fifo_list_);
      key_iter_map_ = std::move(other.key_iter_map_);
    }

    return *this;
  }

  /// @brief Insert or update a key-value pair into the cache (copy semantics).
  /// @param key The key to insert or update.
  /// @param value The value to associate with the key.
  /// @return  true if insertion succeeded, false otherwise (e.g., key exists).
  bool Put(const KeyType& key, const ValueType& value,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) override {
    if (!mutex_.try_lock_for(timeout)) {
      return false;
    }
    std::lock_guard<Mutex> lock(mutex_);

    if (key_iter_map_.find(key) != key_iter_map_.end()) {
      return cache_->Put(key, value);
    }

    if (key_iter_map_.size() >= capacity_) {
      const KeyType& oldest_key = fifo_list_.front();
      key_iter_map_.erase(oldest_key);
      cache_->Remove(oldest_key);

      fifo_list_.front() = key;
      fifo_list_.splice(fifo_list_.end(), fifo_list_, fifo_list_.begin());
    } else {
      fifo_list_.emplace_back(key);
    }

    key_iter_map_[key] = std::prev(fifo_list_.end());

    return cache_->Put(key, value);
  }

  /// @brief Insert or update a key-value pair into the cache (move semantics).
  /// @param key The key to insert or update.
  /// @param value The value to associate with the key.
  /// @return  true if insertion succeeded, false otherwise (e.g., key exists).
  bool Put(const KeyType& key, ValueType&& value,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) override {
    std::lock_guard<Mutex> lock(mutex_);

    if (key_iter_map_.find(key) != key_iter_map_.end()) {
      return cache_->Put(key, std::forward<ValueType>(value));
    }

    if (key_iter_map_.size() >= capacity_) {
      const KeyType& oldest_key = fifo_list_.front();
      key_iter_map_.erase(oldest_key);
      cache_->Remove(oldest_key);

      fifo_list_.front() = key;
      fifo_list_.splice(fifo_list_.end(), fifo_list_, fifo_list_.begin());
    } else {
      fifo_list_.emplace_back(key);
    }

    key_iter_map_[key] = std::prev(fifo_list_.end());

    return cache_->Put(key, std::forward<ValueType>(value));
  }

  /// @brief Retrieves the value associated with the given key.
  /// @param key The key to look up.
  /// @return An optional containing the value if the key exists, std::nullopt otherwise.
  std::optional<ValueType> Get(const KeyType& key,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) override {
    return cache_->Get(key);
  }

  /// @brief Removes the key-value pair associated with the given key.
  /// @param key The key to remove.
  /// @return true if the key was found and removed, false otherwise.
  bool Remove(const KeyType& key, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) override {
    std::lock_guard<Mutex> lock(mutex_);

    auto it = key_iter_map_.find(key);
    if (it == key_iter_map_.end()) {
      return false;
    }

    fifo_list_.erase(it->second);
    key_iter_map_.erase(key);

    return cache_->Remove(key);
  }

  /// @brief Clear all key-value pairs from the cache.
  void Clear() override {
    std::lock_guard<Mutex> lock(mutex_);

    fifo_list_.clear();
    key_iter_map_.clear();
    cache_->Clear();
  }

  /// @brief Get the number of entries currently in the cache.
  /// @return The current size of the cache.
  size_t Size() override { return fifo_list_.size(); }

 private:
  // Maximum number of entries allowed.
  size_t capacity_;
  // The underlying cache implementation.
  std::unique_ptr<Cache<KeyType, ValueType, HashFn, KeyEqual>> cache_;
  // Mutex to ensure thread-safe access to FIFO structures.
  Mutex mutex_;
  // List maintaining insertion order (FIFO eviction).
  std::list<KeyType> fifo_list_;
  // Maps keys to their position in the FIFO list.
  std::unordered_map<KeyType, typename std::list<KeyType>::iterator> key_iter_map_;
};

}  // namespace trpc::cache
