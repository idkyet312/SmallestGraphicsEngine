#ifndef FORWARD_QUALITY_DX12_H
#define FORWARD_QUALITY_DX12_H

#include <algorithm>

// Adaptive Forward Extensions quality.
//
// The stress scene's Forward Extensions pass is dominated by terrain and by
// active destruction, and both scale with how much is happening rather than
// with anything the renderer controls up front. This trades quality for frame
// time only while the pass is genuinely over budget, and only when the toggle
// is on -- with adaptive quality disabled every tier query returns Full, so the
// default frame is bit-identical to the non-adaptive build.
//
// The controller is deliberately asymmetric, the same shape as the destruction
// quality controller it feeds: drop a tier quickly when the pass is expensive,
// recover slowly. A symmetric controller pumps between tiers on a scene that
// sits near a threshold, and tier changes are visible (terrain topology and
// debris counts both change), so pumping is worse than staying one tier low.
enum class ForwardExtensionQualityTier {
    Full = 0,
    Balanced = 1,
    Performance = 2,
};

class ForwardQualityController {
public:
    // Thresholds are on the smoothed pass time, in milliseconds.
    static constexpr float kEnterBalancedMs = 4.0f;
    static constexpr float kEnterPerformanceMs = 5.0f;
    static constexpr float kRecoverMs = 3.2f;
    // Consecutive samples required before acting. Entering is fast enough to
    // catch a collapse as it starts; recovery is slow so a lull mid-collapse
    // does not flip topology back and forth.
    static constexpr int kEnterSamples = 8;
    static constexpr int kRecoverSamples = 120;
    // EMA weight for the new sample. GPU timestamps arrive delayed by frames in
    // flight and are individually noisy, so the raw value is never thresholded.
    static constexpr float kSmoothing = 0.12f;

    void SetEnabled(bool value) {
        if (enabled == value) return;
        enabled = value;
        // Disabling must restore full quality immediately rather than decaying
        // back, so toggling off is an instant, exact return to the default.
        if (!enabled) Reset();
    }
    bool Enabled() const { return enabled; }

    void Reset() {
        tier = ForwardExtensionQualityTier::Full;
        smoothedMs = 0.0f;
        seeded = false;
        overBalanced = overPerformance = underRecover = 0;
    }

    // `sampleMs` is the delayed Forward Extensions GPU time for this frame. A
    // non-positive sample means the timestamp was not resolved (the query was
    // dropped, or the pass did not run), and is ignored rather than treated as
    // a fast frame -- otherwise a missing sample would look like recovery.
    void Update(double sampleMs) {
        if (!enabled) { Reset(); return; }
        if (!(sampleMs > 0.0)) return;
        const float value = static_cast<float>(sampleMs);
        if (!seeded) { smoothedMs = value; seeded = true; }
        else smoothedMs += kSmoothing * (value - smoothedMs);

        if (smoothedMs > kEnterPerformanceMs) ++overPerformance; else overPerformance = 0;
        if (smoothedMs > kEnterBalancedMs) ++overBalanced; else overBalanced = 0;
        if (smoothedMs < kRecoverMs) ++underRecover; else underRecover = 0;

        if (overPerformance >= kEnterSamples &&
            tier != ForwardExtensionQualityTier::Performance) {
            tier = ForwardExtensionQualityTier::Performance;
            ClearCounters();
        } else if (overBalanced >= kEnterSamples &&
                   tier == ForwardExtensionQualityTier::Full) {
            tier = ForwardExtensionQualityTier::Balanced;
            ClearCounters();
        } else if (underRecover >= kRecoverSamples &&
                   tier != ForwardExtensionQualityTier::Full) {
            // One tier at a time, so recovery walks Performance -> Balanced ->
            // Full and each step gets its own settling window.
            tier = (tier == ForwardExtensionQualityTier::Performance)
                ? ForwardExtensionQualityTier::Balanced
                : ForwardExtensionQualityTier::Full;
            ClearCounters();
        }
    }

    ForwardExtensionQualityTier Tier() const {
        return enabled ? tier : ForwardExtensionQualityTier::Full;
    }
    float SmoothedMs() const { return smoothedMs; }

    static const char* TierName(ForwardExtensionQualityTier value) {
        switch (value) {
            case ForwardExtensionQualityTier::Balanced: return "Balanced";
            case ForwardExtensionQualityTier::Performance: return "Performance";
            default: return "Full";
        }
    }

private:
    void ClearCounters() { overBalanced = overPerformance = underRecover = 0; }

    bool enabled = false;
    bool seeded = false;
    ForwardExtensionQualityTier tier = ForwardExtensionQualityTier::Full;
    float smoothedMs = 0.0f;
    int overBalanced = 0;
    int overPerformance = 0;
    int underRecover = 0;
};

inline ForwardQualityController g_forwardQuality;

#endif // FORWARD_QUALITY_DX12_H
