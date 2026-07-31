#ifndef DEFERRED_RELEASE_QUEUE_H
#define DEFERRED_RELEASE_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

template <typename T>
class DeferredReleaseQueue {
public:
    void Retire(uint64_t fenceValue, T value) {
        entries_.push_back({ fenceValue, std::move(value) });
    }

    void Collect(uint64_t completedFenceValue) {
        while (!entries_.empty() &&
               entries_.front().fenceValue <= completedFenceValue)
            entries_.pop_front();
    }

    size_t PendingCount() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t fenceValue;
        T value;
    };
    std::deque<Entry> entries_;
};

#endif
