/**
 */

#pragma once

#include <optional>
#include <chrono>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>

using namespace std;

/**
 * @brief A thread-safe bounded blocking queue.
 *
 * This class provides a multi-producer, multi-consumer FIFO queue with a
 * maximum capacity. Threads attempting to push into a full queue will block
 * until space becomes available. Threads attempting to pop from an empty queue
 * will block until an item is available. The queue can also be stopped to
 * unblock all waiting threads and allow graceful shutdown.
 *
 * @tparam T Type of the items stored in the queue.
 */
template <typename T>
class BlockingQueue {
public:
    /**
     * @brief Constructs a BlockingQueue with a maximum capacity.
     * @param maxSize Maximum number of items the queue can hold.
     */
    explicit BlockingQueue(size_t maxSize) :
        _maxSize(maxSize),
        _stopped(false)
    {}

    /**
     * @brief Pushes an item into the queue.
     * Blocks if the queue is full until space becomes available or the queue
     * is stopped. If the queue is stopped while waiting, the item is discarded.
     * @param item The item to push.
     */
    void push(const T& item)
    {
        unique_lock<mutex> lock(_mtx);
        _condNotFull.wait(lock, [&]{
            return _queue.size() < _maxSize || _stopped;
        });
        if (_stopped) return; // Ignore pushes after stop
        _queue.push(item);
        _condNotEmpty.notify_one();
    }

    /**
     * @brief Pops an item from the queue.
     * Blocks if the queue is empty until an item is available or the queue is
     * stopped. If the queue is stopped and empty, returns std::nullopt.
     * @return optional<T> The popped item, or std::nullopt if the queue
     * was stopped and is empty.
     */
    optional<T> pop()
    {
        unique_lock<mutex> lock(_mtx);
        _condNotEmpty.wait(lock, [&]{return !_queue.empty() || _stopped;});
        if (_queue.empty()) return nullopt; // stopped and no items
        T item = _queue.front();
        _queue.pop();
        _condNotFull.notify_one();
        return item;
    }

    /**
     * @brief Pops an item from the queue with timeout.
     * Blocks until an item becomes available, the queue is stopped, or the
     * timeout expires. If the timeout expires or the queue is stopped while
     * empty, returns std::nullopt.
     * @param timeout Maximum time to wait for an item.
     * @return optional<T> The popped item, or std::nullopt if timed out or
     * stopped.
     */
    optional<T> pop_for(chrono::milliseconds timeout)
    {
        unique_lock<mutex> lock(_mtx);
        bool ready = _condNotEmpty.wait_for(lock, timeout, [&]{
            return !_queue.empty() || _stopped;
        });
        if (!ready || _queue.empty()) return nullopt; // timeout or stopped
        T item = _queue.front();
        _queue.pop();
        _condNotFull.notify_one();
        return item;
    }

    /**
     * @brief Stops the queue and unblocks all waiting operations.
     *
     * After calling stop(), any waiting push() or pop() calls are unblocked.
     * push() will return immediately without inserting items.
     * pop() and pop_for() will return std::nullopt if the queue is empty.
     */
    void stop()
    {
        lock_guard<mutex> lock(_mtx);
        _stopped = true;
        _condNotEmpty.notify_all();
        _condNotFull.notify_all();
    }

private:
    queue<T> _queue; ///< Internal FIFO container.
    size_t _maxSize; ///< Maximum capacity of the queue.
    atomic_bool _stopped; ///< Indicates whether the queue has been stopped.
    mutex _mtx; ///< Mutex protecting shared data.
    condition_variable _condNotEmpty; ///< Signals when items become available.
    condition_variable _condNotFull; ///< Signals when space is available.
};
