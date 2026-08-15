#ifndef WEATHER_H
#define WEATHER_H

#include <DirectXMath.h>
#include <cstdint>

enum class WeatherState : uint8_t {
    Clear = 0,
    Cloudy,
    Fog,
    Rain,
    Storm,
    Custom
};

inline const char* WeatherStateName(WeatherState state) {
    switch (state) {
    case WeatherState::Clear:   return "Clear";
    case WeatherState::Cloudy:  return "Cloudy";
    case WeatherState::Fog:     return "Dense Fog";
    case WeatherState::Rain:    return "Rain";
    case WeatherState::Storm:   return "Storm";
    case WeatherState::Custom:  return "Custom";
    default:                    return "Cloudy";
    }
}

inline const char* WeatherStateBriefing(WeatherState state) {
    switch (state) {
    case WeatherState::Clear:
        return "Dry air and long sight lines; no rain or low cloud.";
    case WeatherState::Cloudy:
        return "Low broken cloud with ordinary jungle haze.";
    case WeatherState::Fog:
        return "Dense ground fog sharply reduces visibility without rain.";
    case WeatherState::Rain:
        return "Steady wind-driven rain under a raised overcast layer.";
    case WeatherState::Storm:
        return "Heavy rain, strong crosswind, low cloud, and short sight lines.";
    case WeatherState::Custom:
        return "Manually tuned rain, cloud, and fog controls.";
    default:
        return "Low broken cloud with ordinary jungle haze.";
    }
}

struct WeatherSettings {
    float rainIntensity = 0.0f;
    DirectX::XMFLOAT2 windVelocity{ 1.6f, 0.7f };

    float skyCloudCoverage = 0.47f;
    float skyCloudDensity = 0.86f;
    float skyCloudBaseHeight = 1240.0f;
    float skyCloudThickness = 1530.0f;

    bool worldClouds = true;
    float worldCloudBaseHeight = 9.0f;
    float worldCloudThickness = 30.0f;
    float worldCloudDensity = 2.25f;
    float worldCloudCoverage = 1.96f;

    bool volumetricFog = true;
    float fogDensity = 0.009f;
    float fogAnisotropy = 0.82f;
    float fogHeightFalloff = 0.045f;
    float fogBaseHeight = 0.4f;
    float fogDistance = 800.0f;
    DirectX::XMFLOAT3 fogTint{
        168.0f / 255.0f, 181.0f / 255.0f, 176.0f / 255.0f
    };
};

inline WeatherSettings MakeWeatherSettings(WeatherState state) {
    WeatherSettings settings;
    switch (state) {
    case WeatherState::Clear:
        settings.windVelocity = { 0.8f, 0.3f };
        settings.skyCloudCoverage = 0.08f;
        settings.skyCloudDensity = 0.32f;
        settings.worldClouds = false;
        settings.volumetricFog = false;
        settings.fogDensity = 0.0025f;
        break;
    case WeatherState::Fog:
        settings.windVelocity = { 0.35f, 0.15f };
        settings.skyCloudCoverage = 0.62f;
        settings.skyCloudDensity = 0.72f;
        settings.worldClouds = false;
        settings.volumetricFog = true;
        settings.fogDensity = 0.020f;
        settings.fogAnisotropy = 0.48f;
        settings.fogHeightFalloff = 0.025f;
        settings.fogBaseHeight = 1.2f;
        settings.fogDistance = 480.0f;
        settings.fogTint = { 0.64f, 0.69f, 0.70f };
        break;
    case WeatherState::Rain:
        settings.rainIntensity = 0.68f;
        settings.windVelocity = { 3.2f, 1.1f };
        settings.skyCloudCoverage = 0.86f;
        settings.skyCloudDensity = 1.08f;
        settings.worldCloudBaseHeight = 65.0f;
        settings.worldCloudThickness = 150.0f;
        settings.worldCloudDensity = 1.35f;
        settings.worldCloudCoverage = 0.88f;
        settings.fogDensity = 0.0115f;
        settings.fogAnisotropy = 0.72f;
        settings.fogHeightFalloff = 0.038f;
        settings.fogDistance = 650.0f;
        settings.fogTint = { 0.58f, 0.64f, 0.66f };
        break;
    case WeatherState::Storm:
        settings.rainIntensity = 1.0f;
        settings.windVelocity = { 6.0f, -2.5f };
        settings.skyCloudCoverage = 1.0f;
        settings.skyCloudDensity = 1.35f;
        settings.worldCloudBaseHeight = 38.0f;
        settings.worldCloudThickness = 230.0f;
        settings.worldCloudDensity = 1.80f;
        settings.worldCloudCoverage = 1.20f;
        settings.fogDensity = 0.0165f;
        settings.fogAnisotropy = 0.68f;
        settings.fogHeightFalloff = 0.030f;
        settings.fogDistance = 500.0f;
        settings.fogTint = { 0.48f, 0.54f, 0.58f };
        break;
    case WeatherState::Cloudy:
    case WeatherState::Custom:
    default:
        break;
    }
    return settings;
}

#endif
