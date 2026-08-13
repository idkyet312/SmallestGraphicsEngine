#ifndef TIME_OF_DAY_H
#define TIME_OF_DAY_H

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>

// Time-of-day presets for the deployment screen. The player picks one before
// the run and it is applied at DEPLOY, so a mission can be flown in daylight or
// under cover of dark.
//
// Deliberately a small set of authored presets rather than a continuous clock:
// every value here (sun angle, colour, intensity, ambient, atmosphere) has to
// look right together, and interpolating a 24-hour cycle through them is a much
// larger job than choosing between four hand-tuned looks. A clock can be built
// on top of this later -- the settings struct is what it would drive.
//
// Kept free of the renderer so the values can be unit-tested: this header is
// pure math and POD, and Scene applies it.
enum class TimeOfDay : uint8_t {
    Noon = 0,
    Afternoon = 1,
    Dusk = 2,
    Night = 3
};

inline const char* TimeOfDayName(TimeOfDay time) {
    switch (time) {
    case TimeOfDay::Noon: return "Noon";
    case TimeOfDay::Afternoon: return "Afternoon";
    case TimeOfDay::Dusk: return "Dusk";
    case TimeOfDay::Night: return "Night";
    default: return "Afternoon";
    }
}

// One-line description for the deployment screen, so the choice reads as a
// tactical trade rather than a graphics setting.
inline const char* TimeOfDayBriefing(TimeOfDay time) {
    switch (time) {
    case TimeOfDay::Noon:
        return "Full light. You will be seen coming.";
    case TimeOfDay::Dusk:
        return "Long shadows, low sun. Good cover on the east approach.";
    case TimeOfDay::Night:
        return "Minimal light. Hard to be seen, hard to see.";
    default:
        return "Angled light, clear visibility.";
    }
}

// Everything a preset changes. Applied wholesale so a switch cannot leave the
// scene in a half-lit mixture of two times of day.
struct TimeOfDaySettings {
    // Direction *to* the sun, unnormalised -- Scene::lightPos is used as a
    // direction by the sky, fog and DDGI, not as a world position.
    DirectX::XMFLOAT3 lightPos{ 4.735f, 3.095f, -8.246f };
    DirectX::XMFLOAT3 lightColor{ 1.0f, 0.92f, 0.70f };
    float directionalLightIntensity = 12.18f;
    float ambientStrength = 0.07f;
    float ambientLightingIntensity = 0.42f;
    DirectX::XMFLOAT3 clearColor{ 0.35f, 0.58f, 0.82f };
    float atmosphereRayleighStrength = 0.92f;
    float atmosphereMieStrength = 0.58f;
    float atmosphereAerialDensity = 0.72f;
    float volumetricFogDensity = 0.009f;
    float volumetricFogAnisotropy = 0.82f;
    float volumetricFogDistance = 800.0f;
    DirectX::XMFLOAT3 volumetricFogTint{
        168.0f / 255.0f, 181.0f / 255.0f, 176.0f / 255.0f
    };
};

// The sun's height above the horizon, as the y of the normalised direction.
// Negative means the sun is down, which is what separates night from dusk.
inline float TimeOfDaySunElevation(const TimeOfDaySettings& settings) {
    const DirectX::XMFLOAT3& l = settings.lightPos;
    const float length = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
    return length > 1e-6f ? l.y / length : 0.0f;
}

// True when the preset is dark enough that the player should expect to rely on
// muzzle flashes and silhouettes rather than seeing across the island.
inline bool TimeOfDayIsDark(TimeOfDay time) {
    return time == TimeOfDay::Night;
}

inline TimeOfDaySettings MakeTimeOfDaySettings(TimeOfDay time) {
    TimeOfDaySettings settings;
    switch (time) {
    case TimeOfDay::Noon:
        // Sun overhead and slightly behind the island's long axis. Near-white,
        // and the sky loses most of its warmth.
        settings.lightPos = { 1.6f, 11.4f, -2.3f };
        settings.lightColor = { 1.0f, 0.97f, 0.92f };
        settings.directionalLightIntensity = 14.6f;
        settings.ambientStrength = 0.085f;
        settings.ambientLightingIntensity = 0.52f;
        settings.clearColor = { 0.38f, 0.62f, 0.88f };
        settings.atmosphereRayleighStrength = 0.98f;
        settings.atmosphereMieStrength = 0.44f;
        settings.atmosphereAerialDensity = 0.60f;
        break;
    case TimeOfDay::Dusk:
        // Sun on the horizon: low elevation for long shadows, and the light
        // swings hard into orange while the sky reddens.
        settings.lightPos = { 9.2f, 0.85f, -3.1f };
        settings.lightColor = { 1.0f, 0.62f, 0.34f };
        settings.directionalLightIntensity = 8.4f;
        settings.ambientStrength = 0.055f;
        settings.ambientLightingIntensity = 0.34f;
        settings.clearColor = { 0.42f, 0.34f, 0.38f };
        settings.atmosphereRayleighStrength = 1.35f;
        settings.atmosphereMieStrength = 0.95f;
        settings.atmosphereAerialDensity = 0.95f;
        break;
    case TimeOfDay::Night:
        // Sun below the horizon (negative y) so the atmosphere renders unlit,
        // with only a trace of cool moonlight. Night is intentionally dominated
        // by silhouettes; local lights, muzzle flashes and the HUD carry detail.
        // Sun well below the horizon: the analytic sky fades its in-scattering
        // out over the last few degrees (see PhysicalSky's sunUp), so this has
        // to clear that band or the horizon keeps a dusk glow.
        settings.lightPos = { -3.4f, -4.2f, 6.8f };
        settings.lightColor = { 0.42f, 0.54f, 0.92f };
        settings.directionalLightIntensity = 0.035f;
        settings.ambientStrength = 0.001f;
        settings.ambientLightingIntensity = 0.004f;
        settings.clearColor = { 0.0002f, 0.0005f, 0.0015f };
        // Thin the atmosphere hard: these scale the in-scattering that would
        // otherwise relight the night HDRI back to dusk.
        settings.atmosphereRayleighStrength = 0.035f;
        settings.atmosphereMieStrength = 0.01f;
        settings.atmosphereAerialDensity = 0.05f;
        // Use the same authored volume as Afternoon. The fog shader supplies
        // night illumination separately, so matching the daytime density,
        // reach, phase and tint restores natural fog without relighting it.
        settings.volumetricFogDensity = 0.009f;
        settings.volumetricFogAnisotropy = 0.82f;
        settings.volumetricFogDistance = 800.0f;
        settings.volumetricFogTint = {
            168.0f / 255.0f, 181.0f / 255.0f, 176.0f / 255.0f
        };
        break;
    case TimeOfDay::Afternoon:
    default:
        // The historical look: this is what every level shipped with before the
        // choice existed, so it stays the default and keeps its exact values.
        break;
    }
    return settings;
}

#endif
