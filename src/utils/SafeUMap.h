#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <functional>

template<typename K, typename V>
class SafeUMap
{
public:
    SafeUMap() = default;
    ~SafeUMap() = default;

    // Insert only if key doesn't exist
    // Returns true if inserted, false if key already exists
    bool emplace(const K& key, std::shared_ptr<V> value) {
        std::unique_lock lock(mutex_);
        return map_.emplace(key, std::move(value)).second;
    }

    // Retrieve shared_ptr (copy) - safe even if erased later
    std::shared_ptr<V> get(const K& key) const {
        std::shared_lock lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end())
            return nullptr;
        return it->second; // shared_ptr keeps the object alive
    }

    // Erase entry
    bool erase(const K& key) {
        std::unique_lock lock(mutex_);
        return map_.erase(key) > 0;
    }

    // Traverse all elements (thread-safe)
    // Callback receives: (const K&, std::shared_ptr<V>)
    template<typename Func>
    void traverse(Func f) const {
        std::shared_lock lock(mutex_);
        for (const auto& kv : map_) {
            f(kv.first, kv.second); // safe: shared_ptr copy
        }
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<K, std::shared_ptr<V>> map_;
};
