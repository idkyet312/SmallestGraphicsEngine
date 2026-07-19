#ifndef PALM_WIND_HLSLI
#define PALM_WIND_HLSLI

float3 PalmRotate(float3 value, float3 axis, float angle) {
    float s, c;
    sincos(angle, s, c);
    return value * c + cross(axis, value) * s + axis * dot(axis, value) * (1.0 - c);
}

void PalmWindVector(float4 palmRoot, float4 windData,
                    float4 primary, float4 secondary, float4 windParams,
                    float localY, out float2 bendVector, out float bendAngle) {
    bendVector = 0.0;
    bendAngle = 0.0;
    if (palmRoot.z < 0.5) return;

    const float heightFraction = saturate(localY / max(windParams.z, 1e-3));
    const float heightWeight = heightFraction * heightFraction;
    const float phase = palmRoot.x * 0.19 + palmRoot.y * 0.13;
    const float time = windData.x;
    const float t = time * windData.w;
    const float gust = sin(t + phase) + 0.35 * sin(t * 2.37 + phase * 1.71);
    const float ambientBend = windData.z * 0.13 * gust * heightWeight;
    const float heading = 0.35 + 0.22 * sin(t * 0.17);
    bendVector = float2(cos(heading), sin(heading)) * ambientBend;

    const float2 treeXZ = palmRoot.xy;
    const float radius = max(windParams.x, 1e-3);
    const float rotorStrength = windParams.y;
    float4 helicopters[2] = { primary, secondary };
    [unroll] for (uint i = 0; i < 2; ++i) {
        if (helicopters[i].w < 0.5) continue;
        const float2 fromHelicopter = treeXZ - helicopters[i].xz;
        const float distance = length(fromHelicopter);
        if (distance >= radius) continue;
        const float falloff = pow(saturate(1.0 - distance / radius), 0.65);
        const float pulse = 0.88 + 0.12 * sin(time * 22.0 + distance * 1.7 + phase);
        const float2 direction = distance > 1e-3
            ? fromHelicopter / distance : float2(1.0, 0.0);
        bendVector += direction * (rotorStrength * falloff * pulse * heightWeight);
    }

    bendAngle = min(0.34, length(bendVector));
}

void ApplyPalmWind(inout float3 position, inout float3 normal,
                   inout float3 tangent, float4 palmRoot, float4 windData,
                   float4 primary, float4 secondary, float4 windParams) {
    // Crown yaw happens before bending. Rotating it in the CPU model matrix
    // rotated the already-bent crown offset and pulled some crowns off trunks.
    if (abs(palmRoot.w) > 1e-5) {
        float s, c;
        sincos(palmRoot.w, s, c);
        position.xz = float2(c * position.x - s * position.z,
                             s * position.x + c * position.z);
        normal.xz = float2(c * normal.x - s * normal.z,
                           s * normal.x + c * normal.z);
        tangent.xz = float2(c * tangent.x - s * tangent.z,
                            s * tangent.x + c * tangent.z);
    }
    float2 bendVector;
    float bendAngle;
    PalmWindVector(palmRoot, windData, primary, secondary, windParams,
                   position.y, bendVector, bendAngle);
    if (bendAngle <= 1e-5) return;
    const float bendMagnitude = max(length(bendVector), 1e-5);
    const float3 axis = float3(bendVector.y / bendMagnitude, 0.0,
                               -bendVector.x / bendMagnitude);
    position = PalmRotate(position, axis, bendAngle);
    normal = normalize(PalmRotate(normal, axis, bendAngle));
    tangent = normalize(PalmRotate(tangent, axis, bendAngle));
}

float3 ApplyPalmWindPosition(float3 position, float4 palmRoot, float4 windData,
                             float4 primary, float4 secondary,
                             float4 windParams) {
    float3 normal = float3(0.0, 1.0, 0.0);
    float3 tangent = float3(1.0, 0.0, 0.0);
    ApplyPalmWind(position, normal, tangent, palmRoot, windData,
                  primary, secondary, windParams);
    return position;
}

#endif
