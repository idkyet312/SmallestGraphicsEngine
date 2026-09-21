#include "TimeOfDay.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

float FadeFor(TimeOfDay time) {
    const TimeOfDaySettings settings = MakeTimeOfDaySettings(time);
    return TimeOfDaySunHorizonFade(TimeOfDaySunElevation(settings));
}
}

// A sun below the horizon must not light the scene. Night deliberately puts the
// sun underneath so the analytic sky renders unlit, and every consumer of the
// directional light -- forward shading, the visibility buffer, the DDGI probes
// and the volumetric fog's in-scattering -- used that direction unguarded. The
// fog sheet over the NATO shelter's floor lit up in the preset's blue as a
// result, with a hard edge where the shadow cascade ran out.
//
// These are the values the daylight presets have always rendered with, so they
// also guard against the fade creeping up over Dusk, which sits close enough to
// the horizon to be the one at risk.
int main() {
    // Night: sun at elevation -0.484, well below the fade's -0.10 floor.
    Check(FadeFor(TimeOfDay::Night) == 0.0f,
          "Night must contribute no direct light");

    // Dusk sits at +0.087, above the +0.06 ceiling, so it keeps its full warm
    // directional light and its long shadows.
    Check(FadeFor(TimeOfDay::Dusk) == 1.0f,
          "Dusk must keep its full directional light");
    Check(FadeFor(TimeOfDay::Noon) == 1.0f,
          "Noon must keep its full directional light");
    Check(FadeFor(TimeOfDay::Afternoon) == 1.0f,
          "Afternoon must keep its full directional light");

    // The curve itself: zero at and below the floor, one at and above the
    // ceiling, monotonic and continuous in between. Matching PhysicalSky's
    // smoothstep(-0.10, 0.06, y) is what keeps the sky, the fog and the scene
    // light crossing the horizon together.
    Check(TimeOfDaySunHorizonFade(-1.0f) == 0.0f, "Fully below is dark");
    Check(TimeOfDaySunHorizonFade(-0.10f) == 0.0f, "Floor is dark");
    Check(TimeOfDaySunHorizonFade(0.06f) == 1.0f, "Ceiling is fully lit");
    Check(TimeOfDaySunHorizonFade(1.0f) == 1.0f, "Overhead is fully lit");
    Check(std::fabs(TimeOfDaySunHorizonFade(-0.02f) - 0.5f) < 1e-5f,
          "Midpoint of the band is half lit");

    float previous = -1.0f;
    for (int step = 0; step <= 64; ++step) {
        const float elevation = -0.20f + 0.40f * (float)step / 64.0f;
        const float fade = TimeOfDaySunHorizonFade(elevation);
        Check(fade >= 0.0f && fade <= 1.0f, "Fade stays within [0,1]");
        Check(fade >= previous - 1e-6f, "Fade never decreases as the sun rises");
        previous = fade;
    }

    if (failures == 0) std::cout << "SunHorizonFadeTests passed\n";
    return failures == 0 ? 0 : 1;
}
