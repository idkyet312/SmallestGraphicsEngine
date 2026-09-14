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

    // UI callbacks can remove an object after its draw was recorded but before
    // that frame has a fence. Keep it until submission is sealed.
    void RetireAfterSubmission(T value) {
        awaitingSubmission_.push_back(std::move(value));
    }

    void SealSubmission(uint64_t fenceValue) {
        for (auto& value : awaitingSubmission_)
            Retire(fenceValue, std::move(value));
        awaitingSubmission_.clear();
    }

    void Collect(uint64_t completedFenceValue) {
        while (!entries_.empty() &&
               entries_.front().fenceValue <= completedFenceValue)
            entries_.pop_front();
    }

    size_t PendingCount() const {
        return entries_.size() + awaitingSubmission_.size();
    }

private:
    struct Entry {
        uint64_t fenceValue;
        T value;
    };
    std::deque<Entry> entries_;
    std::deque<T> awaitingSubmission_;
};

#endif
