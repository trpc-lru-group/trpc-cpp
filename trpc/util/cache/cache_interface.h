#pragma once

namespace trpc::util::cache {
/// @brief Interface of cache.
template <typename Key, typename Value>
class ICache {
  virtual ~ICache() = default;

  /// @brief store key-value pair in cache.
  /// @param key key to be stored.
  /// @param value value to be stored.
  /// @return true if store successfully, false otherwise.
  virtual bool Put(const Key& key, const Value& value) = 0;

  /// @brief get value from cache by key.
  /// @param key key to be searched.
  /// @param value value to be returned.
  /// @return true if get successfully, false otherwise.
  virtual bool Get(const Key& key, Value* out) = 0;

  /// @brief remove key-value pair from cache.
  /// @param key key to be removed.
  /// @return true if remove successfully, false otherwise.
  virtual bool Erase(const Key& key) = 0;

  /// @brief clear all key-value pairs in cache.
  virtual void Clear() = 0;

  /// @brief get size of cache.
  /// @return size of cache.
  virtual size_t Size() const = 0;
};

}  // namespace trpc::util::cache