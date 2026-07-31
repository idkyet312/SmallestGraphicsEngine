#ifndef LEVEL_LOADING_CONTROLLER_H
#define LEVEL_LOADING_CONTROLLER_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class LevelLoadStage {
    WorldAssets,
    Destruction,
    Environment,
    Weapons,
    Humvee,
    Helicopter,
    BanditModel,
    BanditSpawn,
    GPUFinalize,
    ReleaseUploads,
    Complete
};

struct LevelLoadRecord {
    std::string label;
    double milliseconds = 0.0;
    bool succeeded = true;
};

struct LevelLoadRequest {
    uint32_t taskCount = 1;
    std::string firstLabel;
    std::string firstAsset;
};

class LevelLoadingController {
public:
    using Clock = std::chrono::steady_clock;

    void Begin(LevelLoadRequest request, Clock::time_point now = Clock::now()) {
        stage_ = LevelLoadStage::WorldAssets;
        active_ = true;
        progress_ = 0.02f;
        taskCount_ = (std::max)(1u, request.taskCount);
        taskIndex_ = 1;
        label_ = std::move(request.firstLabel);
        asset_ = std::move(request.firstAsset);
        lastSubmittedUploads_ = 0;
        submittedUploads_ = 0;
        startedAt_ = now;
        taskStartedAt_ = now;
        records_.clear();
    }

    void Advance(LevelLoadStage next, std::string label, std::string asset,
                 bool succeeded = true, Clock::time_point now = Clock::now()) {
        RecordCurrent(succeeded, now);
        stage_ = next;
        ++taskIndex_;
        progress_ = (std::min)(0.95f,
            static_cast<float>(taskIndex_ - 1) /
            static_cast<float>(taskCount_));
        label_ = std::move(label);
        asset_ = std::move(asset);
        taskStartedAt_ = now;
    }

    void Complete(bool succeeded = true,
                  Clock::time_point now = Clock::now()) {
        RecordCurrent(succeeded, now);
        stage_ = LevelLoadStage::Complete;
        taskIndex_ = taskCount_;
        progress_ = 1.0f;
        label_ = "Ready";
        asset_ = "None";
        active_ = false;
    }

    void SetCurrent(std::string label, std::string asset) {
        label_ = std::move(label);
        asset_ = std::move(asset);
    }

    void RecordSubmittedUploads(uint32_t count) {
        lastSubmittedUploads_ = count;
        submittedUploads_ += count;
    }

    bool Active() const { return active_; }
    LevelLoadStage Stage() const { return stage_; }
    float Progress() const { return progress_; }
    uint32_t TaskCount() const { return taskCount_; }
    uint32_t TaskIndex() const { return taskIndex_; }
    uint32_t LastSubmittedUploads() const { return lastSubmittedUploads_; }
    uint32_t SubmittedUploads() const { return submittedUploads_; }
    const std::string& Label() const { return label_; }
    const std::string& Asset() const { return asset_; }
    const std::vector<LevelLoadRecord>& Records() const { return records_; }

    double TotalElapsedMilliseconds(
        Clock::time_point now = Clock::now()) const {
        return ElapsedMilliseconds(startedAt_, now);
    }

    double TaskElapsedMilliseconds(
        Clock::time_point now = Clock::now()) const {
        return ElapsedMilliseconds(taskStartedAt_, now);
    }

private:
    static double ElapsedMilliseconds(Clock::time_point start,
                                      Clock::time_point now) {
        return std::chrono::duration<double, std::milli>(now - start).count();
    }

    void RecordCurrent(bool succeeded, Clock::time_point now) {
        records_.push_back(
            { label_, ElapsedMilliseconds(taskStartedAt_, now), succeeded });
        if (records_.size() > 6)
            records_.erase(records_.begin());
    }

    LevelLoadStage stage_ = LevelLoadStage::Complete;
    bool active_ = false;
    float progress_ = 0.0f;
    uint32_t taskCount_ = 1;
    uint32_t taskIndex_ = 0;
    uint32_t lastSubmittedUploads_ = 0;
    uint32_t submittedUploads_ = 0;
    std::string label_ = "Preparing level...";
    std::string asset_ = "None";
    Clock::time_point startedAt_ = Clock::now();
    Clock::time_point taskStartedAt_ = startedAt_;
    std::vector<LevelLoadRecord> records_;
};

#endif
